// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
// SPDX-FileCopyrightText: Copyright 2018 Intel Corporation
// SPDX-FileCopyrightText: Copyright 2018 Ampere Computing LLC

#pragma once

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/power.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "sensors.hpp"
#include "utils/chassis_utils.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/sensor_utils.hpp"

#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace redfish
{

// Power capping settings live on xyz.openbmc_project.Settings, one object per
// host instance. The host->socket binding is owned by the power-capping
// daemon; bmcweb only needs to select the correct settings object:
//   2P   (HostMode CurrentMode == 0) -> host0 controls all sockets
//   2x1P (HostMode CurrentMode == 1) -> host1 = P0, host2 = P1 (independent)
constexpr int hparMode2P = 0;
constexpr int hparMode2x1P = 1;

inline std::string powerCapObjectPath(int hostInstance)
{
    return "/xyz/openbmc_project/control/host" + std::to_string(hostInstance) +
           "/power_cap";
}

// Ordered list of host instances exposed as PowerControl members for a mode.
// Index in this list == PowerControl MemberId.
inline std::vector<int> powerCapHostsForMode(int mode)
{
    if (mode == hparMode2x1P)
    {
        return {1, 2}; // PowerControl/0 -> host1 (P0), PowerControl/1 -> host2
    }
    return {0}; // 2P / default: single PowerControl -> host0
}

// Read HostMode CurrentMode and invoke cb(mode). Defaults to 2P (0) on any
// error or when HostMode is unavailable, so single-socket / legacy 2P
// platforms keep the existing single-host behavior. CurrentMode encoding is
// accepted as integer-like or numeric string to match the platform tooling.
inline void getHparModeThen(std::function<void(int)>&& cb)
{
    dbus::utility::getAllProperties(
        "xyz.openbmc_project.Settings",
        "/xyz/openbmc_project/control/HostMode",
        "xyz.openbmc_project.Control.HostMode",
        [cb = std::move(cb)](
            const boost::system::error_code& ec,
            const dbus::utility::DBusPropertiesMap& properties) mutable {
            int mode = hparMode2P;
            if (ec)
            {
                BMCWEB_LOG_DEBUG("HostMode unavailable, assuming 2P: {}",
                                 ec.message());
                cb(mode);
                return;
            }
            for (const std::pair<std::string, dbus::utility::DbusVariantType>&
                     property : properties)
            {
                if (property.first != "CurrentMode")
                {
                    continue;
                }
                if (const std::string* s =
                        std::get_if<std::string>(&property.second))
                {
                    try
                    {
                        mode = std::stoi(*s);
                    }
                    catch (...)
                    {
                        mode = hparMode2P;
                    }
                }
                else if (const uint8_t* u8 =
                             std::get_if<uint8_t>(&property.second))
                {
                    mode = *u8;
                }
                else if (const uint16_t* u16 =
                             std::get_if<uint16_t>(&property.second))
                {
                    mode = *u16;
                }
                else if (const uint32_t* u32 =
                             std::get_if<uint32_t>(&property.second))
                {
                    mode = static_cast<int>(*u32);
                }
                else if (const uint64_t* u64 =
                             std::get_if<uint64_t>(&property.second))
                {
                    mode = static_cast<int>(*u64);
                }
                else if (const int64_t* i64 =
                             std::get_if<int64_t>(&property.second))
                {
                    mode = static_cast<int>(*i64);
                }
                else if (const bool* b = std::get_if<bool>(&property.second))
                {
                    mode = *b ? hparMode2x1P : hparMode2P;
                }
            }
            cb(mode);
        });
}

inline void afterGetPowerCapEnable(
    const std::shared_ptr<SensorsAsyncResp>& sensorsAsyncResp, int hostInstance,
    uint32_t valueToSet, const boost::system::error_code& ec,
    bool powerCapEnable)
{
    if (ec)
    {
        messages::internalError(sensorsAsyncResp->asyncResp->res);
        BMCWEB_LOG_ERROR("powerCapEnable Get handler: Dbus error {}", ec);
        return;
    }
    if (!powerCapEnable)
    {
        messages::actionNotSupported(
            sensorsAsyncResp->asyncResp->res,
            "Setting LimitInWatts when PowerLimit feature is disabled");
        BMCWEB_LOG_ERROR("PowerLimit feature is disabled ");
        return;
    }

    setDbusProperty(
        sensorsAsyncResp->asyncResp, "PowerControl", "xyz.openbmc_project.Settings",
        sdbusplus::message::object_path(powerCapObjectPath(hostInstance)),
        "xyz.openbmc_project.Control.Power.Cap", "PowerCap", valueToSet);
}

// Apply a single PowerControl member's PowerLimit to the given host instance's
// power_cap settings object. Mirrors the legacy single-host semantics:
//   LimitInWatts == 0 / null -> disable cap (PowerCapEnable = false)
//   LimitInWatts  > 0        -> enable cap and set PowerCap
inline void applyPowerCapMember(
    const std::shared_ptr<SensorsAsyncResp>& sensorsAsyncResp, int hostInstance,
    nlohmann::json::object_t item)
{
    std::optional<uint32_t> value;
    if (!item.contains("PowerLimit") ||
        !item["PowerLimit"].contains("LimitInWatts") ||
        item["PowerLimit"]["LimitInWatts"].is_null())
    {
        value = 0;
    }
    else if (!json_util::readJsonObject(item, sensorsAsyncResp->asyncResp->res,
                                        "PowerLimit/LimitInWatts", value))
    {
        return;
    }

    sdbusplus::message::object_path objPath(powerCapObjectPath(hostInstance));

    /* d-bus exposes two properties, PowerCapEnable and PowerCap.
       But redfish(UI) uses only one PowerCap value.
       So, update feature enable/disable flag based on PowerCapValue
       if PowerCapValue is 0, disable power-cap feature
       if non-zero, enable power-cap and set PowerCapValue.
    */
    if (value == 0)
    {
        setDbusProperty(sensorsAsyncResp->asyncResp, "PowerControl",
                        "xyz.openbmc_project.Settings", objPath, "xyz.openbmc_project.Control.Power.Cap",
                        "PowerCapEnable", false);
        sensorsAsyncResp->asyncResp->res.result(
            boost::beast::http::status::no_content);
        return;
    }

    setDbusProperty(sensorsAsyncResp->asyncResp, "PowerControl",
                    "xyz.openbmc_project.Settings", objPath, "xyz.openbmc_project.Control.Power.Cap",
                    "PowerCapEnable", true);

    dbus::utility::getProperty<bool>(
        "xyz.openbmc_project.Settings", powerCapObjectPath(hostInstance),
        "xyz.openbmc_project.Control.Power.Cap", "PowerCapEnable",
        std::bind_front(afterGetPowerCapEnable, sensorsAsyncResp, hostInstance,
                        *value));
}

inline void afterGetChassisPath(
    const std::shared_ptr<SensorsAsyncResp>& sensorsAsyncResp,
    std::vector<nlohmann::json::object_t> powerControlCollections,
    const std::optional<std::string>& chassisPath)
{
    if (!chassisPath)
    {
        BMCWEB_LOG_WARNING("Don't find valid chassis path ");
        messages::resourceNotFound(sensorsAsyncResp->asyncResp->res, "Chassis",
                                   sensorsAsyncResp->chassisId);
        return;
    }

    getHparModeThen([sensorsAsyncResp, powerControlCollections =
                                           std::move(powerControlCollections)](
                        int mode) {
        std::vector<int> hosts = powerCapHostsForMode(mode);

        // One PowerControl member per host this mode exposes. Reject more
        // members than the mode supports (e.g. 2 controls in 2P).
        if (powerControlCollections.empty() ||
            powerControlCollections.size() > hosts.size())
        {
            BMCWEB_LOG_WARNING(
                "Unexpected PowerControl count {} for {} host(s) ",
                powerControlCollections.size(), hosts.size());
            messages::resourceNotFound(sensorsAsyncResp->asyncResp->res,
                                       "Power", "PowerControl");
            return;
        }

        for (size_t i = 0; i < powerControlCollections.size(); ++i)
        {
            nlohmann::json::object_t item = powerControlCollections[i];

            // Route by MemberId when the client provides it, so a single
            // PowerControl member can target one specific CPU/host in 2x1P
            // (e.g. PATCH only MemberId "1" -> host2) without touching the
            // other CPU. Fall back to array position so the legacy
            // single-host PATCH (no MemberId) keeps working.
            size_t memberIndex = i;
            auto memberIt = item.find("MemberId");
            if (memberIt != item.end())
            {
                const std::string* memberId =
                    memberIt->second.get_ptr<const std::string*>();
                if (memberId == nullptr)
                {
                    messages::propertyValueTypeError(
                        sensorsAsyncResp->asyncResp->res, memberIt->second,
                        "MemberId");
                    return;
                }
                size_t parsed = 0;
                try
                {
                    parsed = static_cast<size_t>(std::stoul(*memberId));
                }
                catch (...)
                {
                    messages::propertyValueFormatError(
                        sensorsAsyncResp->asyncResp->res, *memberId,
                        "MemberId");
                    return;
                }
                memberIndex = parsed;

                // Drop MemberId before handing the object to
                // applyPowerCapMember: its readJsonObject only consumes
                // PowerLimit/LimitInWatts and would reject MemberId as an
                // unrecognized property (HTTP 400).
                item.erase("MemberId");
            }

            if (memberIndex >= hosts.size())
            {
                messages::propertyValueOutOfRange(
                    sensorsAsyncResp->asyncResp->res,
                    std::to_string(memberIndex), "MemberId");
                return;
            }

            applyPowerCapMember(sensorsAsyncResp, hosts[memberIndex],
                                std::move(item));
        }
    });
}

inline void afterPowerCapSettingGet(
    const std::shared_ptr<SensorsAsyncResp>& sensorAsyncResp,
    size_t memberIndex, bool multiHost, const boost::system::error_code& ec,
    const dbus::utility::DBusPropertiesMap& properties)
{
    if (ec)
    {
        if (memberIndex == 0)
        {
            messages::internalError(sensorAsyncResp->asyncResp->res);
        }
        BMCWEB_LOG_ERROR("Power Limit GetAll handler: Dbus error {}", ec);
        return;
    }

    nlohmann::json& tempArray =
        sensorAsyncResp->asyncResp->res.jsonValue["PowerControl"];

    // Ensure a PowerControl member exists at memberIndex. In 2P there is a
    // single member (index 0, shared with the chassis power sensors). In 2x1P
    // each CPU gets its own member.
    while (tempArray.size() <= memberIndex)
    {
        size_t idx = tempArray.size();
        nlohmann::json::object_t powerControl;
        powerControl["@odata.type"] = "#Power.v1_0_0.PowerControl";
        powerControl["@odata.id"] =
            "/redfish/v1/Chassis/" + sensorAsyncResp->chassisId +
            "/Power#/PowerControl/" + std::to_string(idx);
        powerControl["MemberId"] = std::to_string(idx);
        if (multiHost)
        {
            powerControl["Name"] = "CPU " + std::to_string(idx) +
                                   " Power Control";
        }
        else
        {
            powerControl["Name"] = "Chassis Power Control";
        }
        tempArray.emplace_back(std::move(powerControl));
    }

    nlohmann::json& sensorJson = tempArray[memberIndex];
    bool enabled = false;
    double powerCap = 0.0;
    int64_t scale = 0;

    for (const std::pair<std::string, dbus::utility::DbusVariantType>&
             property : properties)
    {
        if (property.first == "Scale")
        {
            const int64_t* i = std::get_if<int64_t>(&property.second);

            if (i != nullptr)
            {
                scale = *i;
            }
        }
        else if (property.first == "PowerCap")
        {
            const double* d = std::get_if<double>(&property.second);
            const int64_t* i = std::get_if<int64_t>(&property.second);
            const uint32_t* u = std::get_if<uint32_t>(&property.second);

            if (d != nullptr)
            {
                powerCap = *d;
            }
            else if (i != nullptr)
            {
                powerCap = static_cast<double>(*i);
            }
            else if (u != nullptr)
            {
                powerCap = *u;
            }
        }
        else if (property.first == "PowerCapEnable")
        {
            const bool* b = std::get_if<bool>(&property.second);

            if (b != nullptr)
            {
                enabled = *b;
            }
        }
    }

    // LimitException is Mandatory attribute as per OCP
    // Baseline Profile - v1.0.0, so currently making it
    // "NoAction" as default value to make it OCP Compliant.
    sensorJson["PowerLimit"]["LimitException"] =
        power::PowerLimitException::NoAction;

    if (enabled)
    {
        // Redfish specification indicates PowerLimit should
        // be null if the limit is not enabled.
        sensorJson["PowerLimit"]["LimitInWatts"] =
            powerCap * std::pow(10, scale);
    }
}

using Mapper = dbus::utility::MapperGetSubTreePathsResponse;
inline void afterGetChassis(
    const std::shared_ptr<SensorsAsyncResp>& sensorAsyncResp,
    const boost::system::error_code& ec2, const Mapper& chassisPaths)
{
    if (ec2)
    {
        BMCWEB_LOG_ERROR("Power Limit GetSubTreePaths handler Dbus error {}",
                         ec2);
        return;
    }

    bool found = false;
    for (const std::string& chassis : chassisPaths)
    {
        size_t len = std::string::npos;
        size_t lastPos = chassis.rfind('/');
        if (lastPos == std::string::npos)
        {
            continue;
        }

        if (lastPos == chassis.size() - 1)
        {
            size_t end = lastPos;
            lastPos = chassis.rfind('/', lastPos - 1);
            if (lastPos == std::string::npos)
            {
                continue;
            }

            len = end - (lastPos + 1);
        }

        std::string interfaceChassisName = chassis.substr(lastPos + 1, len);
        if (interfaceChassisName == sensorAsyncResp->chassisId)
        {
            found = true;
            break;
        }
    }

    if (!found)
    {
        BMCWEB_LOG_DEBUG("Power Limit not present for {}",
                         sensorAsyncResp->chassisId);
        return;
    }

    // Expose one PowerControl member per host the running HPAR mode owns.
    getHparModeThen([sensorAsyncResp](int mode) {
        std::vector<int> hosts = powerCapHostsForMode(mode);
        bool multiHost = hosts.size() > 1;
        for (size_t i = 0; i < hosts.size(); ++i)
        {
            size_t memberIndex = i;
            dbus::utility::getAllProperties(
                "xyz.openbmc_project.Settings", powerCapObjectPath(hosts[i]),
                "xyz.openbmc_project.Control.Power.Cap",
                [sensorAsyncResp, memberIndex, multiHost](
                    const boost::system::error_code& ec,
                    const dbus::utility::DBusPropertiesMap& properties) {
                    afterPowerCapSettingGet(sensorAsyncResp, memberIndex,
                                            multiHost, ec, properties);
                });
        }
    });
}

inline void handleChassisPowerGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    asyncResp->res.jsonValue["PowerControl"] = nlohmann::json::array();

    auto sensorAsyncResp = std::make_shared<SensorsAsyncResp>(
        asyncResp, chassisName, sensors::dbus::powerPaths,
        sensor_utils::chassisSubNodeToString(
            sensor_utils::ChassisSubNode::powerNode));

    getChassisData(sensorAsyncResp);

    // This callback verifies that the power limit is only provided
    // for the chassis that implements the Chassis inventory item.
    // This prevents things like power supplies providing the
    // chassis power limit

    dbus::utility::getSubTreePaths(
        "/xyz/openbmc_project/inventory", 0, chassisInterfaces,
        std::bind_front(afterGetChassis, sensorAsyncResp));
}

inline void handleChassisPowerPatch(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    auto sensorAsyncResp = std::make_shared<SensorsAsyncResp>(
        asyncResp, chassisName, sensors::dbus::powerPaths,
        sensor_utils::chassisSubNodeToString(
            sensor_utils::ChassisSubNode::powerNode));

    std::optional<std::vector<nlohmann::json::object_t>> voltageCollections;
    std::optional<std::vector<nlohmann::json::object_t>> powerCtlCollections;

    if (!json_util::readJsonPatch(                //
            req, sensorAsyncResp->asyncResp->res, //
            "PowerControl", powerCtlCollections,  //
            "Voltages", voltageCollections        //
            ))
    {
        return;
    }

    if (powerCtlCollections)
    {
        redfish::chassis_utils::getValidChassisPath(
            sensorAsyncResp->asyncResp, sensorAsyncResp->chassisId,
            std::bind_front(afterGetChassisPath, sensorAsyncResp,
                            *powerCtlCollections));
    }
    if (voltageCollections)
    {
        std::unordered_map<std::string, std::vector<nlohmann::json::object_t>>
            allCollections;
        allCollections.emplace("Voltages", std::move(*voltageCollections));
        setSensorsOverride(sensorAsyncResp, allCollections);
    }
}

inline void requestRoutesPower(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Power/")
        .privileges(redfish::privileges::getPower)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleChassisPowerGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Chassis/<str>/Power/")
        .privileges(redfish::privileges::patchPower)
        .methods(boost::beast::http::verb::patch)(
            std::bind_front(handleChassisPowerPatch, std::ref(app)));
}

} // namespace redfish
