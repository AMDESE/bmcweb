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
#include "http_utility.hpp"
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
// host instance. The caller selects which host to act on with a ?HostNumber=
// URL parameter (consistent with the Systems power-on / reset flow):
//   HostNumber absent / 0 -> host0 (2P: single host controls all sockets)
//   HostNumber 1          -> host1 (2x1P: CPU 0 / P0)
//   HostNumber 2          -> host2 (2x1P: CPU 1 / P1)
// The host->socket binding is owned by the power-capping daemon; bmcweb only
// needs to select the correct settings object for the requested host.
constexpr uint8_t maxPowerCapHostNumber = 2;

inline std::string powerCapObjectPath(uint8_t hostInstance)
{
    return "/xyz/openbmc_project/control/host" +
           std::to_string(static_cast<unsigned>(hostInstance)) + "/power_cap";
}

inline void afterGetPowerCapEnable(
    const std::shared_ptr<SensorsAsyncResp>& sensorsAsyncResp,
    uint8_t hostInstance, uint32_t valueToSet,
    const boost::system::error_code& ec, bool powerCapEnable)
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
    const std::shared_ptr<SensorsAsyncResp>& sensorsAsyncResp,
    uint8_t hostInstance, nlohmann::json::object_t item)
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
    uint8_t hostInstance,
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

    // The target host is selected by the ?HostNumber= URL parameter, so the
    // body carries a single PowerControl member describing the cap to apply.
    if (powerControlCollections.size() != 1)
    {
        BMCWEB_LOG_WARNING("Expected exactly one PowerControl member, got {} ",
                           powerControlCollections.size());
        messages::resourceNotFound(sensorsAsyncResp->asyncResp->res, "Power",
                                   "PowerControl");
        return;
    }

    nlohmann::json::object_t item = powerControlCollections[0];

    // MemberId (if a client still sends it) no longer selects the host; the
    // host is chosen by ?HostNumber=. Drop it before applyPowerCapMember, whose
    // readJsonObject only consumes PowerLimit/LimitInWatts and would otherwise
    // reject MemberId as an unrecognized property (HTTP 400).
    item.erase("MemberId");

    applyPowerCapMember(sensorsAsyncResp, hostInstance, std::move(item));
}

inline void afterPowerCapSettingGet(
    const std::shared_ptr<SensorsAsyncResp>& sensorAsyncResp,
    uint8_t hostInstance, const boost::system::error_code& ec,
    const dbus::utility::DBusPropertiesMap& properties)
{
    if (ec)
    {
        messages::internalError(sensorAsyncResp->asyncResp->res);
        BMCWEB_LOG_ERROR("Power Limit GetAll handler: Dbus error {}", ec);
        return;
    }

    nlohmann::json& tempArray =
        sensorAsyncResp->asyncResp->res.jsonValue["PowerControl"];

    // The target host is selected by ?HostNumber=, so a single PowerControl
    // member is exposed. Put multiple "sensors" into that single member.
    if (tempArray.empty())
    {
        nlohmann::json::object_t powerControl;
        powerControl["@odata.type"] = "#Power.v1_0_0.PowerControl";
        powerControl["@odata.id"] =
            "/redfish/v1/Chassis/" + sensorAsyncResp->chassisId +
            "/Power#/PowerControl/0";
        powerControl["MemberId"] = "0";
        if (hostInstance != 0)
        {
            powerControl["Name"] =
                "CPU " + std::to_string(static_cast<unsigned>(hostInstance) - 1) +
                " Power Control";
        }
        else
        {
            powerControl["Name"] = "Chassis Power Control";
        }
        tempArray.emplace_back(std::move(powerControl));
    }

    nlohmann::json& sensorJson = tempArray.back();
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
    uint8_t hostInstance, const boost::system::error_code& ec2,
    const Mapper& chassisPaths)
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

    // Expose the PowerControl member for the host selected by ?HostNumber=.
    dbus::utility::getAllProperties(
        "xyz.openbmc_project.Settings", powerCapObjectPath(hostInstance),
        "xyz.openbmc_project.Control.Power.Cap",
        [sensorAsyncResp, hostInstance](
            const boost::system::error_code& ec,
            const dbus::utility::DBusPropertiesMap& properties) {
            afterPowerCapSettingGet(sensorAsyncResp, hostInstance, ec,
                                    properties);
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

    // Host selection is consistent with the Systems power-on flow: the caller
    // appends ?HostNumber=1 or ?HostNumber=2 to target a specific CPU/host in
    // 2x1P; no HostNumber (0) targets host0 (2P / single-host default).
    uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
    if (hostNumber > maxPowerCapHostNumber)
    {
        messages::actionParameterNotSupported(
            asyncResp->res, std::to_string(hostNumber), "HostNumber");
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
        std::bind_front(afterGetChassis, sensorAsyncResp, hostNumber));
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

    // Host selection is consistent with the Systems power-on flow: the caller
    // appends ?HostNumber=1 or ?HostNumber=2 to target a specific CPU/host in
    // 2x1P; no HostNumber (0) targets host0 (2P / single-host default).
    uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
    if (hostNumber > maxPowerCapHostNumber)
    {
        messages::actionParameterNotSupported(
            asyncResp->res, std::to_string(hostNumber), "HostNumber");
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
            std::bind_front(afterGetChassisPath, sensorAsyncResp, hostNumber,
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
