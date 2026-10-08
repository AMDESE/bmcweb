// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

// AMD HPAR (2x1P) host-resolution helper for the Redfish host interface.
//
// BIOS exchanges CBS/PCIe/DIMM/Storage data with the BMC over a per-host USB
// vNIC. In 2x1P the iodevices-inventory producer runs one instance per host and
// publishes under a per-host object root:
//   host0 : /xyz/openbmc_project/inventory            (2P, legacy, unchanged)
//   hostN : /xyz/openbmc_project/inventory/hostN       (2x1P, N = 1 | 2)
//
// The target host for a request is resolved WITHOUT any BIOS-side URI change:
//   1. An explicit "?HostNumber=N" query parameter wins (for out-of-band /
//      management-LAN clients that cannot come in over a vNIC).
//   2. Otherwise the host is inferred from the source vNIC the request arrived
//      on (usb0 192.168.31.x, usb1 192.168.32.x) - this is the BIOS path and
//      needs no BIOS change.
//   3. Otherwise host0 (single-host / 2P), so 2P behaviour is byte-identical.
//
// GET reads use hostInventoryRoot()/resolveReadHost() to scope the ObjectMapper
// search to the resolved host. host0 keeps the original inventory root, so 1P/2P
// read behaviour is completely unchanged.

#include "async_resp.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"

#include <boost/url/url_view.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace redfish::amd_hpar
{

// Inventory D-Bus root (matches iodevices-inventory kInventoryRoot /
// pcieMainPath base and bmcweb's inventoryPath).
inline constexpr const char* kInventoryRoot = "/xyz/openbmc_project/inventory";

// True when the BMC is running in single-host / 2P mode (host0 partition).
// multi-host-config writes host0.conf for 2P and host1.conf/host2.conf for
// 2x1P, and the iodevices-inventory / usb-network instances are all gated on
// the same files, so this is the authoritative mode selector.
inline bool is2pMode()
{
    std::error_code ec;
    return std::filesystem::exists("/etc/amd/hosts.d/host0.conf", ec);
}

// Map the USB vNIC a request arrived on to a host instance. Mirrors
// amd-ipmi-oem resolveHostInterface():
//   usb0 192.168.31.x -> P0 -> host0 in 2P, host1 in 2x1P
//   usb1 192.168.32.x -> P1 -> host2
// Returns 0 for any other interface (eth0, loopback, ...) so normal Redfish
// clients are unaffected.
inline uint8_t hostNumberFromVnic(const crow::Request& req)
{
    const std::string ip = req.ipAddress.to_string();
    if (ip.rfind("192.168.31.", 0) == 0 ||
        ip.find("192.168.31.") != std::string::npos)
    {
        return is2pMode() ? 0 : 1;
    }
    if (ip.find("192.168.32.") != std::string::npos)
    {
        return 2;
    }
    return 0;
}

// Result of resolving the target host for a request.
//   hostNumber : 0 (host0/2P), 1 (host1/P0) or 2 (host2/P1)
//   ambiguous  : true only in 2x1P when the request carried no explicit
//                HostNumber AND did not arrive over a vNIC, so host0 cannot be
//                assumed. Callers should reject such reads with a 4xx instead
//                of silently returning the wrong / empty host.
struct HostResolution
{
    uint8_t hostNumber;
    bool ambiguous;
};

// Resolve the target host following the precedence described at the top of the
// file (explicit HostNumber -> source vNIC -> host0), and flag the 2x1P
// ambiguous case.
inline HostResolution resolveHost(const crow::Request& req)
{
    boost::urls::url_view urlView = req.url();
    for (const auto& param : urlView.params())
    {
        if (param.key == "HostNumber" && !param.value.empty())
        {
            try
            {
                int temp = std::stoi(std::string(param.value));
                if (temp >= 0 && temp <= 2)
                {
                    return {static_cast<uint8_t>(temp), false};
                }
            }
            catch (const std::exception&)
            {
                // fall through: treat malformed value as legacy host0
            }
            // Out-of-range / malformed explicit value -> legacy host0.
            return {0, false};
        }
    }

    // No explicit HostNumber: infer from the source vNIC.
    uint8_t vnicHost = hostNumberFromVnic(req);
    if (vnicHost != 0)
    {
        return {vnicHost, false};
    }
    // vNIC inference returned 0: fine in 2P (host0 is the only host), but in
    // 2x1P there is no host0 and we cannot guess between host1/host2.
    if (is2pMode())
    {
        return {0, false};
    }
    return {0, true};
}

// Thin wrapper kept for the write path (POST/PATCH) and callers that do not
// need the ambiguity flag. Mirrors the single-value resolver used elsewhere.
inline uint8_t hostNumberFromReq(const crow::Request& req)
{
    return resolveHost(req).hostNumber;
}

// ObjectMapper search root for the resolved host.
//   host0 -> the base inventory root (unchanged 1P/2P behaviour)
//   hostN -> the per-host sub-tree, so GET reads only see that host's objects.
inline std::string hostInventoryRoot(uint8_t hostNumber)
{
    if (hostNumber == 0)
    {
        return std::string{kInventoryRoot};
    }
    return std::string{kInventoryRoot} + "/host" + std::to_string(hostNumber);
}

// Resolve the host for a GET read, emitting a clear 400 and returning false
// when the request is ambiguous (2x1P, no HostNumber, not from a vNIC).
inline bool resolveReadHost(const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            uint8_t& hostNumber)
{
    HostResolution res = resolveHost(req);
    if (res.ambiguous)
    {
        messages::queryParameterOutOfRange(
            asyncResp->res, "absent", "HostNumber",
            "1-2 (required to select a host in 2x1P/HPAR mode)");
        return false;
    }
    hostNumber = res.hostNumber;
    return true;
}

// iodevices-inventory well-known service name for the given host (write path /
// direct D-Bus calls). Kept here for parity with the per-host producer naming.
inline std::string pcieDataService(uint8_t hostNumber)
{
    if (hostNumber == 0)
    {
        return "xyz.openbmc_project.PCIe";
    }
    return "xyz.openbmc_project.PCIe.host" + std::to_string(hostNumber);
}

// Object that hosts the PcieData interface for the given host.
inline std::string pcieDataObject(uint8_t hostNumber)
{
    if (hostNumber == 0)
    {
        return "/xyz/openbmc_project/inventory/PCIe";
    }
    return "/xyz/openbmc_project/inventory/host" + std::to_string(hostNumber) +
           "/PCIe";
}

} // namespace redfish::amd_hpar
