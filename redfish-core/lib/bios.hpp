// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/sw_utils.hpp"

#include <boost/beast/http/verb.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>
namespace redfish
{
/**
 * BiosService class supports handle get method for bios.
 */
using BiosAttrMap = std::map<std::string, std::variant<int64_t, std::string>>;
// Convert json object to key value pair
inline BiosAttrMap JsonToBiosAttributes(const nlohmann::json& j)
{
    BiosAttrMap iMap;
    for (auto& [key, value] : j.items())
    {
        if (value.is_number_integer())
        {
            iMap[key] = value.get<int64_t>(); // Store as int64_t
        }
        else if (value.is_string())
        {
            iMap[key] = value.get<std::string>(); // Store as string
        }
        else
        {
            iMap[key] = std::string(value);
        }
    } // end of for loop

    return iMap;
}

/**
 * @brief  Check for allowed IP Addresses for usbvNic interface
 *
 **/
inline bool isAllowedIpAddress(const crow::Request& req)
{
    const std::string ipStr = req.ipAddress.to_string();

    constexpr std::string_view usbVnic1 = "::ffff:192.168.31.1";
    constexpr std::string_view usbVnic2 = "::ffff:192.168.31.2";

    if (ipStr == usbVnic1 || ipStr == usbVnic2)
    {
        BMCWEB_LOG_WARNING(
                "Reject request from usbvNIC");
        return false;
    }

    return true;
}

/**
 * @brief  Retrieves all CBS attributes data over DBus function
 *
 **/
inline void handleBiosServiceGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
    {
        // Option currently returns no systems.  TBD
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }
    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] = std::format(
        "/redfish/v1/Systems/{}/Bios", BMCWEB_REDFISH_SYSTEM_URI_NAME);
    asyncResp->res.jsonValue["@odata.type"] = "#Bios.v1_1_0.Bios";
    asyncResp->res.jsonValue["@Redfish.Settings"] = {
        {"@odata.type", "#Settings.v1_3_3.Settings"}};
    asyncResp->res.jsonValue["@Redfish.Settings"]["SettingsObject"] = {
        {"@odata.id", "/redfish/v1/Systems/system/Bios/SD"}};
    asyncResp->res.jsonValue["Name"] = "BIOS Configuration";
    asyncResp->res.jsonValue["Description"] = "BIOS Configuration Service";
    asyncResp->res.jsonValue["Id"] = "BIOS";
    asyncResp->res.jsonValue["Actions"]["#Bios.ResetBios"]["target"] =
        std::format("/redfish/v1/Systems/{}/Bios/Actions/Bios.ResetBios",
                    BMCWEB_REDFISH_SYSTEM_URI_NAME);

    if (isAllowedIpAddress(req))
    {
        // Get the ActiveSoftwareImage and SoftwareImages
        sw_util::populateSoftwareInformation(asyncResp, sw_util::biosPurpose, "",
                                          true);
    }

    asyncResp->res.jsonValue["Attributes"] = nlohmann::json::object();

    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec, BiosAttrMap& newtable) {
            if (ec)
            {
                BMCWEB_LOG_ERROR(
                    "GET - GetBiosAttribute D-Bus responses error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            nlohmann::json jsonObject;
            for (const auto& [key, value] : newtable)
            {
                std::visit([&jsonObject,
                            &key](const auto& val) { jsonObject[key] = val; },
                           value);
            }
            if (!jsonObject.is_null())
            {
                asyncResp->res.jsonValue["Attributes"] = jsonObject;
            }
            messages::success(asyncResp->res);
            return;
        },
        "xyz.openbmc_project.PCIe", "/xyz/openbmc_project/inventory/PCIe",
        "xyz.openbmc_project.PCIe.PcieData", "GetBiosAttribute");
}

inline void setPendingAttributes(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const BiosAttrMap& patchMap)
{
    // now do the get the persistent value
    crow::connections::systemBus->async_method_call(
        [asyncResp, patchMap](const boost::system::error_code ec,
                              BiosAttrMap& PendingAttrData) {
            if (ec)
            {
                BMCWEB_LOG_ERROR(
                    "setPendingAttributes - patch Get D-Bus responses error: {}",
                    ec);
                messages::internalError(asyncResp->res);
                return;
            }

            for (const auto& [key, value] : patchMap)
            {
                // This will insert the key if it doesn't exist,
                // or update the value if it does.
                PendingAttrData[key] = value;
            }

            // make dbus call transfer the data
            crow::connections::systemBus->async_method_call(
                [asyncResp](const boost::system::error_code ec1) {
                    if (ec1)
                    {
                        BMCWEB_LOG_ERROR(
                            "setPendingAttributes - D-Bus responses error: {}",
                            ec1);
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    BMCWEB_LOG_DEBUG("setPendingAttributes -  Dbus call pass ");
                    messages::success(asyncResp->res);
                    return;
                },
                "xyz.openbmc_project.PCIe",
                "/xyz/openbmc_project/inventory/PCIe",
                "xyz.openbmc_project.PCIe.PcieData", "SetPendingAttribute",
                PendingAttrData);
        },
        "xyz.openbmc_project.PCIe", "/xyz/openbmc_project/inventory/PCIe",
        "xyz.openbmc_project.PCIe.PcieData", "GetPendingAttribute");
}

/**
 * @brief Persist all CBS attributes data over DBus function
 *
 **/
inline void setBiosAttributes(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const BiosAttrMap& table)
{
    // make dbus call transfer the data
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG("SetBiosAttribute D-Bus responses error: {}",
                                 ec);
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
            asyncResp->res.jsonValue["status"] = "ok";
            return;
        },
        "xyz.openbmc_project.PCIe", "/xyz/openbmc_project/inventory/PCIe",
        "xyz.openbmc_project.PCIe.PcieData", "SetBiosAttribute", table);
}

/**
 * @brief Serve Patch request on CBS attributes data over DBus
 *
 **/
inline void handleBiosServicePatch(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }
    // validation -check if any CBS is available to do patch
    nlohmann::json bJsonPatchObject =
        nlohmann::json::parse(req.body(), nullptr, false);
    if (!bJsonPatchObject.contains("Attributes"))
    {
        asyncResp->res.jsonValue["message"] = "Not valid input";
        asyncResp->res.jsonValue["status"] = "error";
        return;
    }
    // validation -user should select atleast one cbs attributes for
    // modification
    BiosAttrMap patchMap = JsonToBiosAttributes(bJsonPatchObject["Attributes"]);
    if (patchMap.size() == 0)
    {
        asyncResp->res.jsonValue["message"] = "Not valid input";
        asyncResp->res.jsonValue["status"] = "error";
        return;
    }

    // now do the get the persistent value
    crow::connections::systemBus->async_method_call(
        [asyncResp,
         patchMap](const boost::system::error_code ec, BiosAttrMap& allData) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG(
                    "PATCH - GetBiosAttribute D-Bus responses error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            // first get all cbs data
            if (allData.size() == 0)
            {
                asyncResp->res.jsonValue["message"] = "No CBS data to patch";
                asyncResp->res.jsonValue["status"] = "error";
                return;
            }
            bool status = true;
            // validation - Iterate through all keys from patchMap
            // and check if key present or not in original cbs data
            for (const auto& [key, value] : patchMap)
            {
                // If key exists in allData, update it
                if (allData.find(key) != allData.end())
                {
                    allData[key] = value;
                }
                else
                {
                    status = false;
                }
            }
            if (status)
            {
                // setBiosAttributes(asyncResp, allData);
                setPendingAttributes(asyncResp, patchMap);
                messages::success(asyncResp->res);
                asyncResp->res.jsonValue["status"] = "ok";
            }
            else
            {
                asyncResp->res.jsonValue["message"] = "Invalid Key to patch";
                asyncResp->res.jsonValue["status"] = "error";
            }
        },
        "xyz.openbmc_project.PCIe", "/xyz/openbmc_project/inventory/PCIe",
        "xyz.openbmc_project.PCIe.PcieData", "GetBiosAttribute");
}

/**
 * @brief Serve POST request on CBS attributes data over DBus
 *
 **/
inline void handleBiosServicePost(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
    {
        // Option currently returns no systems.  TBD
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    nlohmann::json biosPostJsonObject =
        nlohmann::json::parse(req.body(), nullptr, false);
    if (!biosPostJsonObject.contains("Attributes"))
    {
        asyncResp->res.jsonValue["message"] = "Not valid input";
        return;
    }

    // call function with Param in
    BiosAttrMap table = JsonToBiosAttributes(biosPostJsonObject["Attributes"]);
    // make dbus call transfer the data
    setBiosAttributes(asyncResp, table);
}

/**
 * @brief Serve PUT request on CBS attributes data over DBus
 *
 **/
inline void handleBiosServicePut(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
    {
        // Option currently returns no systems.  TBD
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    nlohmann::json biosPostJsonObject =
        nlohmann::json::parse(req.body(), nullptr, false);
    if (!biosPostJsonObject.contains("Attributes"))
    {
        asyncResp->res.jsonValue["message"] = "Not valid input";
        return;
    }

    // call function with Param in
    BiosAttrMap table = JsonToBiosAttributes(biosPostJsonObject["Attributes"]);
    // make dbus call transfer the data
    setBiosAttributes(asyncResp, table);
}

inline void requestRoutesBiosService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios/")
        .privileges(redfish::privileges::getBios)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleBiosServiceGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios")
        .privileges(redfish::privileges::patchBios)
        .methods(boost::beast::http::verb::patch)(
            std::bind_front(handleBiosServicePatch, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios")
        .privileges(redfish::privileges::postBios)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleBiosServicePost, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios")
        .privileges(redfish::privileges::putBios)
        .methods(boost::beast::http::verb::put)(
            std::bind_front(handleBiosServicePut, std::ref(app)));
}

/**
 * BiosReset class supports handle POST method for Reset bios.
 * The class retrieves and sends data directly to D-Bus.
 *
 * Function handles POST method request.
 * Analyzes POST body message before sends Reset request data to D-Bus.
 */
inline void handleBiosResetPost(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
    {
        // Option currently returns no systems.  TBD
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }

    std::string hostStr = "0";
    boost::urls::url_view urlView = req.url();

    // Extract HostNumber from the parameter list
    for (const auto& param : urlView.params())
    {
        if (param.key == "HostNumber" && !param.value.empty())
            try
            {
                hostStr = std::string(param.value);
            }
            catch (const std::exception& e)
            {
                BMCWEB_LOG_WARNING("Invalid HostNumber format: {}",
                                   param.value);
                return;
            }
    }

    uint8_t hostNumber = 0;
    try
    {
        hostNumber = static_cast<uint8_t>(std::stoul(hostStr));
        if (hostNumber > 3)
        {
            BMCWEB_LOG_WARNING("HostNumber value is out of range");
            return;
        }
    }
    catch (std::exception& e)
    {
        BMCWEB_LOG_WARNING("Invalid HostNumber format");
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp, hostNumber](const boost::system::error_code& ec) {
            if (ec)
            {
                std::string cmd =
                    std::format("/sbin/amd-clear-cmos.sh Y {} &", hostNumber);
                int systemRet = system(cmd.c_str());
                if (systemRet == -1)
                {
                    BMCWEB_LOG_ERROR("Failed to clear CMOS");
                    messages::internalError(asyncResp->res);
                }
                return;
            }
        },
        "org.open_power.Software.Host.Updater", "/xyz/openbmc_project/software",
        "xyz.openbmc_project.Common.FactoryReset", "Reset");
}

inline void requestRoutesBiosReset(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios/Actions/Bios.ResetBios/")
        .privileges(redfish::privileges::postBios)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleBiosResetPost, std::ref(app)));
}

/**
 *  BiosSettingsGet method to support settings for pending cbs
 */
inline void handleBiosSettingsGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
    {
        // Option currently returns no systems.  TBD
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }
    if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                   systemName);
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] = std::format(
        "/redfish/v1/Systems/{}/Bios/SD", BMCWEB_REDFISH_SYSTEM_URI_NAME);
    asyncResp->res.jsonValue["@odata.type"] = "#Bios.v1_1_0.Bios";
    asyncResp->res.jsonValue["Id"] = "BIOS";
    asyncResp->res.jsonValue["Name"] = "BIOS Configuration";
    asyncResp->res.jsonValue["Attributes"] = nlohmann::json::object();
    // now do the get the persistent value
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec,
                    BiosAttrMap& PendingAttrData) {
            if (ec)
            {
                BMCWEB_LOG_ERROR(
                    "setPendingAttributes - patch Get D-Bus responses error: {}",
                    ec);
                messages::internalError(asyncResp->res);
                return;
            }

            nlohmann::json jsonObject;
            for (const auto& [key, value] : PendingAttrData)
            {
                std::visit([&jsonObject,
                            &key](const auto& val) { jsonObject[key] = val; },
                           value);
            }
            if (!jsonObject.is_null())
            {
                asyncResp->res.jsonValue["Attributes"] = jsonObject;
            }
            messages::success(asyncResp->res);
            return;
        },
        "xyz.openbmc_project.PCIe", "/xyz/openbmc_project/inventory/PCIe",
        "xyz.openbmc_project.PCIe.PcieData", "GetPendingAttribute");

    return;
}

inline void requestRoutesBiosSettings(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/Bios/SD")
        .privileges(redfish::privileges::getBios)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleBiosSettingsGet, std::ref(app)));
}

} // namespace redfish
