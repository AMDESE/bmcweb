// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

// AMD HPAR (2x1P) helper.
//
// bmcweb exchanges CBS/PCIe/DIMM/Storage data with BIOS through the
// iodevices-inventory "PcieData" D-Bus interface. In 2x1P that service runs as
// one instance per host, so the target service name and object path depend on
// which host the request is for. The host is selected by the "?HostNumber=N"
// query parameter (the same idiom already used for RAS, power-cap and
// clear-CMOS).
//
// HostNumber 0 or absent keeps the legacy single-host (2P / host0) service name
// and object path, so existing 2P behaviour is completely unchanged. Hosts 1
// and 2 map to the two independent iodevices-inventory instances in 2x1P and
// mirror iodevices-inventory's hostServiceName()/hostInventoryPath():
//   host0 : xyz.openbmc_project.PCIe        /xyz/openbmc_project/inventory/PCIe
//   hostN : xyz.openbmc_project.PCIe.hostN  /xyz/openbmc_project/inventory/hostN/PCIe

#include "http_request.hpp"

#include <boost/url/url_view.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace redfish::amd_hpar
{

// True when the BMC is running in single-host / 2P mode (host0 partition).
// multi-host-config writes host0.conf for 2P and host1.conf/host2.conf for
// 2x1P, and the iodevices-inventory / power-control / usb-network instances are
// all gated on the same files, so this is the authoritative mode selector.
inline bool is2pMode()
{
    std::error_code ec;
    return std::filesystem::exists("/etc/amd/hosts.d/host0.conf", ec);
}

// Map the USB vNIC a request arrived on to a host instance. BIOS pushes
// CBS/PCIe/DIMM/Storage data through the Redfish host interface over a per-host
// vNIC and cannot set the "?HostNumber" query parameter, so the host has to be
// inferred from the source address. This mirrors amd-ipmi-oem
// resolveHostInterface():
//   usb0 192.168.31.x -> P0 -> host0 in 2P, host1 in 2x1P
//   usb1 192.168.32.x -> P1 -> host2
// Returns 0 for any other interface (eth0, loopback, ...) so normal Redfish
// clients are unaffected.
inline uint8_t hostNumberFromVnic(const crow::Request& req)
{
    const std::string ip = req.ipAddress.to_string();
    if (ip.find("192.168.31.") != std::string::npos)
    {
        return is2pMode() ? 0 : 1;
    }
    if (ip.find("192.168.32.") != std::string::npos)
    {
        return 2;
    }
    return 0;
}

// Resolve the target host for a request.
//  1. An explicit "?HostNumber=N" (1..2) always wins - lets a normal Redfish
//     client target a specific host in 2x1P.
//  2. Otherwise fall back to the source vNIC so BIOS pushes (which have no
//     HostNumber) reach the right per-host iodevices-inventory instance in
//     2x1P without any BIOS-side change.
// Absent/invalid/out-of-range and non-vNIC sources => 0 (single-host / 2P), so
// existing 2P behaviour is completely unchanged.
inline uint8_t hostNumberFromReq(const crow::Request& req)
{
    boost::urls::url_view urlView = req.url();
    for (const auto& param : urlView.params())
    {
        if (param.key == "HostNumber" && !param.value.empty())
        {
            try
            {
                int temp = std::stoi(std::string(param.value));
                if (temp > 0 && temp <= 2)
                {
                    return static_cast<uint8_t>(temp);
                }
            }
            catch (const std::exception&)
            {
                return 0;
            }
            return 0;
        }
    }
    return hostNumberFromVnic(req);
}

// iodevices-inventory well-known service name for the given host.
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
