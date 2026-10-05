// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright Advanced Micro Devices, Inc.
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/log_service.hpp"
#include "http_request.hpp"
#include "http_utility.hpp"
#include "logging.hpp"
#include "log_services.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "task.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/time_utils.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/linux_error.hpp>
#include <boost/url/format.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/unpack_properties.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace redfish
{

constexpr const char* amdCrashdumpObject = "com.amd.RAS";
constexpr const char* amdCrashdumpPath = "/com/amd/RAS";
constexpr const char* amdCrashdumpInterface = "com.amd.crashdump";
constexpr const char* amdTracelogPath = "/com/amd/Traces";
constexpr const char* amdDeleteAllInterface =
    "xyz.openbmc_project.Collection.DeleteAll";
constexpr const char* amdPprFileObject =
    "xyz.openbmc_project.PostPackageRepair";
constexpr const char* amdPprFilePath =
    "/xyz/openbmc_project/PostPackageRepair";
constexpr const char* amdPprFileInterface =
    "xyz.openbmc_project.PostPackageRepair.PprData";

enum class AttributeType
{
    Boolean,
    String,
    Integer,
    ArrayOfStrings,
    KeyValueMap,
};

using ConfigTable =
    std::map<std::string,
             std::tuple<std::string, std::string,
                        std::variant<bool, std::string, int64_t,
                                     std::vector<std::string>,
                                     std::map<std::string, std::string>>,
                        int64_t>>;

inline void requestRoutesAmdCrashdumpService(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Crashdump/")
        // This is incorrect, should be:
        //.privileges(redfish::privileges::getLogService)
        .privileges({{"ConfigureManager"}})
        .methods(
            boost::beast::http::verb::
                get)([&app](const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& systemName) {
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

            // Copy over the static data to include the entries added by
            // SubRoute
            asyncResp->res.jsonValue["@odata.id"] =
                std::format("/redfish/v1/Systems/{}/LogServices/Crashdump",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogService.v1_2_0.LogService";
            asyncResp->res.jsonValue["Name"] = "Open BMC Oem Crashdump Service";
            asyncResp->res.jsonValue["Description"] = "Oem Crashdump Service";
            asyncResp->res.jsonValue["Id"] = "Crashdump";
            asyncResp->res.jsonValue["OverWritePolicy"] =
                log_service::OverWritePolicy::WrapsWhenFull;
            asyncResp->res.jsonValue["MaxNumberOfRecords"] = 10;

            std::pair<std::string, std::string> redfishDateTimeOffset =
                redfish::time_utils::getDateTimeOffsetNow();
            asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
            asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                redfishDateTimeOffset.second;

            asyncResp->res.jsonValue["Entries"]["@odata.id"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/Crashdump/Entries",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]
                                    ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/LogService.ClearLog",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res
                .jsonValue["Actions"]["#LogService.CollectDiagnosticData"]
                          ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/LogService.CollectDiagnosticData",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["Actions"]["#Oem/Crashdump.Configuration"]
                                    ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/Oem/Crashdump.Configuration",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
        });
}
inline void requestRoutesAmdCrashdumpClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/Crashdump/Actions/LogService.ClearLog/")
        // This is incorrect, should be:
        //.privileges(redfish::privileges::postLogService)
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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

                uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
                if (hostNumber > 2)
                {
                    messages::actionParameterNotSupported(
                        asyncResp->res, std::to_string(hostNumber),
                        "HostNumber");
                    return;
                }
                std::string service =
                    "com.amd.RAS" + std::to_string(hostNumber);

                crow::connections::systemBus->async_method_call(
                    [asyncResp, service](const boost::system::error_code& ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                    },
                    service, amdCrashdumpPath, amdDeleteAllInterface, "DeleteAll");
            });
}

inline void amdLogCrashdumpEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& logID, uint16_t hostNumber, nlohmann::json& logEntryJson)
{
    std::string serviceName = "com.amd.RAS" + std::to_string(hostNumber);

    auto getStoredLogCallback =
        [asyncResp, logID, &logEntryJson, hostNumber,
         serviceName](const boost::system::error_code& ec,
                      const dbus::utility::DBusPropertiesMap& params) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG("failed to get log ec: {}", ec.message());
                if (ec.value() ==
                    boost::system::linux_error::bad_request_descriptor)
                {
                    messages::resourceNotFound(asyncResp->res, "LogEntry",
                                               logID);
                }
                else
                {
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            std::string timestamp{};
            std::string filename{};
            [[maybe_unused]] std::string logfile{};
            parseCrashdumpParameters(params, filename, timestamp, logfile);

            if (filename.empty() || timestamp.empty())
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            std::string crashdumpURI;

            if (hostNumber != 0)
            {
                crashdumpURI =
                    std::format(
                        "/redfish/v1/Systems/{}/LogServices/Crashdump/Entries/",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                    logID + "/" + filename +
                    "?HostNumber=" + std::to_string(hostNumber);
            }
            else
            {
                crashdumpURI =
                    std::format(
                        "/redfish/v1/Systems/{}/LogServices/Crashdump/Entries/",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                    logID + "/" + filename;
            }
            nlohmann::json::object_t logEntry;
            logEntry["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
            logEntry["@odata.id"] = boost::urls::format(
                "/redfish/v1/Systems/{}/LogServices/Crashdump/Entries/{}",
                BMCWEB_REDFISH_SYSTEM_URI_NAME, logID);
            logEntry["Name"] = "CPU Crashdump";
            logEntry["Id"] = logID;
            logEntry["EntryType"] = "Oem";
            logEntry["AdditionalDataURI"] = std::move(crashdumpURI);
            logEntry["DiagnosticDataType"] = "OEM";
            logEntry["Created"] = std::move(timestamp);

            std::string DiagnosticDataTypeString;

            if (filename.find("mca-runtime") != std::string::npos)
                DiagnosticDataTypeString = "Mca_RuntimeError_APMLCrashdump";
            else if (filename.find("dram-runtime") != std::string::npos)
                DiagnosticDataTypeString =
                    "DramCecc_RuntimeError_APMLCrashdump";
            else if (filename.find("pcie-runtime") != std::string::npos)
                DiagnosticDataTypeString = "Pcie_RuntimeError_APMLCrashdump";
            else
                DiagnosticDataTypeString = "PECICrashdump";

            logEntry["DiagnosticDataTypeString"] = DiagnosticDataTypeString;

            // If logEntryJson references an array of LogEntry resources
            // ('Members' list), then push this as a new entry, otherwise set it
            // directly
            if (logEntryJson.is_array())
            {
                logEntryJson.push_back(logEntry);
                asyncResp->res.jsonValue["Members@odata.count"] =
                    logEntryJson.size();
            }
            else
            {
                logEntryJson.update(logEntry);
            }
        };
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, serviceName,
        amdCrashdumpPath + std::string("/") + logID, amdCrashdumpInterface,
        std::move(getStoredLogCallback));
}

static void logTraceLogEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& logID, [[maybe_unused]] uint16_t hostNumber,
    nlohmann::json& logEntryJson)
{
    // DBus service name
    std::string serviceName = "com.amd.Traces";

    constexpr std::string_view TracedumpInterface =
        "com.amd.crashdump";

    auto getStoredLogCallback =
        [asyncResp, logID, &logEntryJson](
            const boost::system::error_code& ec,
            const dbus::utility::DBusPropertiesMap& params)
    {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("Failed to get log {}: {}", logID, ec.message());
            return;
        }

        std::string timestamp{};
        std::string filename{};
        std::string logfile{};

        parseCrashdumpParameters(params, filename, timestamp, logfile);

        //Debug (VERY useful)
        BMCWEB_LOG_DEBUG("Parsed filename={}, timestamp={}, logfile={}",
                         filename, timestamp, logfile);

        if (filename.empty() || timestamp.empty())
        {
            BMCWEB_LOG_DEBUG("Skipping entry {} due to missing fields", logID);
            return;
        }

        std::string tracelogURI =
                std::format(
                    "/redfish/v1/Systems/{}/LogServices/TraceLogs/Entries/",
                    BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                    logID + "/" + filename;

        nlohmann::json::object_t logEntry;

        logEntry["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
        logEntry["@odata.id"] = boost::urls::format(
            "/redfish/v1/Systems/{}/LogServices/TraceLogs/Entries/{}",
            BMCWEB_REDFISH_SYSTEM_URI_NAME, logID);

        logEntry["Name"] = "Distributed Trace Buffer TraceLogs";
        logEntry["Id"] = logID;
        logEntry["EntryType"] = "Oem";
        logEntry["AdditionalDataURI"] = std::move(tracelogURI);
        logEntry["DiagnosticDataType"] = "OEM";
        logEntry["Created"] = std::move(timestamp);
        std::string oemDiagnosticDataType = logID;
        std::transform(oemDiagnosticDataType.begin(),
                       oemDiagnosticDataType.end(),
                       oemDiagnosticDataType.begin(),
                       [](unsigned char ch) { return std::toupper(ch); });
        logEntry["OEMDiagnosticDataType"] = std::move(oemDiagnosticDataType);

        if (logEntryJson.is_array())
        {
            logEntryJson.push_back(logEntry);
        }
        else
        {
            logEntryJson.update(logEntry);
        }
    };

    // FINAL DBus call
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus,
        serviceName,
        std::string(amdTracelogPath) + "/" + logID,
        std::string(TracedumpInterface),
        std::move(getStoredLogCallback));
}

inline void requestRoutesAmdCrashdumpEntryCollection(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/Crashdump/Entries/")
        // This is incorrect, should be.
        //.privileges(redfish::privileges::postLogEntryCollection)
        .privileges({{"ConfigureComponents"}})
        .methods(
            boost::beast::http::verb::
                get)([&app](const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& systemName) {
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

            uint16_t hostNumber = http_helpers::getHostNumberFromUrl(req);

            if (hostNumber > 2)
            {
                messages::actionParameterNotSupported(
                    asyncResp->res, std::to_string(hostNumber), "HostNumber");
                return;
            }

            constexpr std::array<std::string_view, 1> interfaces = {
                amdCrashdumpInterface};
            dbus::utility::getSubTreePaths(
                "/", 0, interfaces,
                [asyncResp, hostNumber](const boost::system::error_code& ec,
                                        const std::vector<std::string>& resp) {
                    if (ec)
                    {
                        if (ec.value() !=
                            boost::system::errc::no_such_file_or_directory)
                        {
                            BMCWEB_LOG_DEBUG("failed to get entries ec: {}",
                                             ec.message());
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    asyncResp->res.jsonValue["@odata.type"] =
                        "#LogEntryCollection.LogEntryCollection";
                    asyncResp->res.jsonValue["@odata.id"] = std::format(
                        "/redfish/v1/Systems/{}/LogServices/Crashdump/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
                    asyncResp->res.jsonValue["Name"] =
                        "Open BMC Crashdump Entries";
                    asyncResp->res.jsonValue["Description"] =
                        "Collection of Crashdump Entries";
                    asyncResp->res.jsonValue["Members"] =
                        nlohmann::json::array();
                    asyncResp->res.jsonValue["Members@odata.count"] = 0;

                    for (const std::string& path : resp)
                    {
			if (path.find(amdTracelogPath) != std::string::npos)
				continue;

                        const sdbusplus::message::object_path objPath(path);
                        // Get the log ID
                        std::string logID = objPath.filename();
                        if (logID.empty())
                        {
                            continue;
                        }
                        // Add the log entry to the array
                        amdLogCrashdumpEntry(asyncResp, logID, hostNumber,
                                          asyncResp->res.jsonValue["Members"]);
                    }
                });
        });
}

inline void requestRoutesAmdCrashdumpEntry(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/Crashdump/Entries/<str>/")
        // this is incorrect, should be
        // .privileges(redfish::privileges::getLogEntry)
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& param) {
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
                    uint16_t hostNumber = http_helpers::getHostNumberFromUrl(req);
                ;

                if (hostNumber > 2)
                {
                    messages::actionParameterNotSupported(
                        asyncResp->res, std::to_string(hostNumber),
                        "HostNumber");
                    return;
                }

                const std::string& logID = param;
                amdLogCrashdumpEntry(asyncResp, logID, hostNumber,
                                  asyncResp->res.jsonValue);
            });
}

inline void requestRoutesAmdCrashdumpFile(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/Crashdump/Entries/<str>/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& systemName, const std::string& logID,
               const std::string& fileName) {
                // Do not call getRedfishRoute here since the crashdump file is
                // not a Redfish resource.

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

                    uint16_t hostNumber = http_helpers::getHostNumberFromUrl(req);

                if (hostNumber > 2)
                {
                    messages::actionParameterNotSupported(
                        asyncResp->res, std::to_string(hostNumber),
                        "HostNumber");
                    return;
                }

                std::string serviceName =
                    "com.amd.RAS" + std::to_string(hostNumber);

                auto getStoredLogCallback =
                    [asyncResp, logID, fileName,
                     url(boost::urls::url(req.url()))](
                        const boost::system::error_code& ec,
                        const std::vector<std::pair<
                            std::string, dbus::utility::DbusVariantType>>&
                            resp) {
                        if (ec)
                        {
                            BMCWEB_LOG_CRITICAL("failed to get log ec: {}",
                                                ec.message());
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        std::string dbusFilename{};
                        std::string dbusTimestamp{};
                        std::string dbusFilepath{};

                        parseCrashdumpParameters(resp, dbusFilename,
                                                 dbusTimestamp, dbusFilepath);

                        if (dbusFilename.empty() || dbusTimestamp.empty() ||
                            dbusFilepath.empty())
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", logID);
                            return;
                        }

                        // Verify the file name parameter is correct
                        if (fileName != dbusFilename)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", logID);
                            return;
                        }

                        if (asyncResp->res.openFile(dbusFilepath) !=
                            crow::OpenCode::Success)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", logID);
                            return;
                        }

                        // Configure this to be a file download when accessed
                        // from a browser
                        asyncResp->res.addHeader(
                            boost::beast::http::field::content_disposition,
                            "attachment");
                    };
                sdbusplus::asio::getAllProperties(
                    *crow::connections::systemBus, serviceName,
                    amdCrashdumpPath + std::string("/") + logID,
                    amdCrashdumpInterface, std::move(getStoredLogCallback));
            });
}

inline void requestRoutesCrashdumpConfig(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Crashdump/"
                      "Actions/Oem/Crashdump.Configuration")
        .privileges({{"ConfigureComponents"}})
        .methods(
            boost::beast::http::verb::
                get)([&app](const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& systemName) {
            if (!redfish::setUpRedfishRoute(app, req, asyncResp))
            {
                BMCWEB_LOG_ERROR("Failed to setup Redfish route");
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

            uint16_t hostNumber = http_helpers::getHostNumberFromUrl(req);

            if (hostNumber > 2)
            {
                messages::actionParameterNotSupported(
                    asyncResp->res, std::to_string(hostNumber), "HostNumber");
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#LogService.v1_2_0.LogService";
            asyncResp->res.jsonValue["@odata.id"] =
                std::format("/redfish/v1/Systems/{}/LogServices/Crashdump/"
                            "Actions/Oem/Crashdump.Configuration",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME);

            std::string serviceName =
                "com.amd.RAS" + std::to_string(hostNumber);

            sdbusplus::asio::getProperty<ConfigTable>(
                *crow::connections::systemBus, serviceName, "/com/amd/RAS",
                "com.amd.RAS.Configuration", "RasConfigTable",
                [asyncResp](const boost::system::error_code& ec,
                            const ConfigTable& rasConfigTable) {
                    if (ec)
                    {
                        BMCWEB_LOG_DEBUG("DBUS RAS Config response error {}",
                                         ec);
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    nlohmann::json jsonConfigTable = nlohmann::json::object();

                    for (const auto& [key, tuple] : rasConfigTable)
                    {
                        const auto& variant =
                            std::get<2>(tuple); // Extract the variant (third
                                                // element of the tuple)

                        std::visit(
                            [&jsonConfigTable, &key](auto&& value) {
                                using T = std::decay_t<
                                    decltype(value)>; // Get the actual type of
                                                      // the value
                                if constexpr (std::is_same_v<T, bool>)
                                {
                                    jsonConfigTable[key] =
                                        value; // Handle bool type
                                }
                                else if constexpr (std::is_same_v<T,
                                                                  std::string>)
                                {
                                    jsonConfigTable[key] =
                                        value; // Handle std::string type
                                }
                                else if constexpr (std::is_same_v<T, int64_t>)
                                {
                                    jsonConfigTable[key] =
                                        value; // Handle int64_t type
                                }
                                else if constexpr (
                                    std::is_same_v<T, std::vector<std::string>>)
                                {
                                    jsonConfigTable[key] =
                                        value; // Handle vector<std::string>
                                               // type
                                }
                                else if constexpr (std::is_same_v<
                                                       T,
                                                       std::map<std::string,
                                                                std::string>>)
                                {
                                    jsonConfigTable[key] =
                                        value; // Handle map<string, string>
                                               // type
                                }
                                else
                                {
                                    BMCWEB_LOG_DEBUG(
                                        "Unknown variant type encountered.");
                                }
                            },
                            variant);
                    }
                    asyncResp->res.jsonValue["ConfigTable"] = jsonConfigTable;
                });
        });

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Crashdump/"
                      "Actions/Oem/Crashdump.Configuration")
        .privileges({{"ConfigureComponents"}})
        .methods(
            boost::beast::http::verb::
                patch)([&app](
                           const crow::Request& req,
                           const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& systemName) {
            if (!redfish::setUpRedfishRoute(app, req, asyncResp))
            {
                BMCWEB_LOG_ERROR("Failed to setup Redfish route");
                return;
            }
            if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
            {
                // Option currently returns no systems.  TBD
                messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                           systemName);
                return;
            }

            uint16_t hostNumber = http_helpers::getHostNumberFromUrl(req);

            if (hostNumber > 2)
            {
                messages::actionParameterNotSupported(
                    asyncResp->res, std::to_string(hostNumber), "HostNumber");
                return;
            }

            std::string serviceName =
                "com.amd.RAS" + std::to_string(hostNumber);

            std::optional<std::map<std::string, std::string>>
                aifsSignatureIdList;
            std::optional<int64_t> apmlRetries;
            std::optional<std::string> SystemRecoveryMode;
            std::optional<std::string> ResetSignalType;
            std::optional<bool> HarvestMicrocode;
            std::optional<bool> HarvestPPIN;
            std::optional<std::vector<std::string>> SigIdOffset;
            std::optional<bool> aifsArmed;
            std::optional<bool> DisableAifsResetOnSyncfloodCounter;
            std::optional<bool> DramCeccPollingEn;
            std::optional<bool> McaPollingEn;
            std::optional<bool> PcieAerPollingEn;
            std::optional<bool> DramCeccThresholdEn;
            std::optional<bool> McaThresholdEn;
            std::optional<bool> PcieAerThresholdEn;
            std::optional<int64_t> McaPollingPeriod;
            std::optional<int64_t> DramCeccPollingPeriod;
            std::optional<int64_t> PcieAerPollingPeriod;
            std::optional<int64_t> DramCeccErrThresholdCnt;
            std::optional<int64_t> McaErrThresholdCnt;
            std::optional<int64_t> McaUmcErrThresholdCnt;
            std::optional<int64_t> PcieAerErrThresholdCnt;
	    std::optional<std::vector<std::string>> Soc0MPList;
	    std::optional<std::vector<std::string>> Soc1MPList;

            if (!redfish::json_util::readJsonAction(
                    req, asyncResp->res, "AifsSignatureIdList",
                    aifsSignatureIdList, "ApmlRetries", apmlRetries,
                    "SystemRecoveryMode", SystemRecoveryMode, "ResetSignalType",
                    ResetSignalType, "HarvestMicrocode", HarvestMicrocode,
                    "HarvestPPIN", HarvestPPIN, "SigIdOffset", SigIdOffset,
                    "AifsArmed", aifsArmed,
                    "DisableAifsResetOnSyncfloodCounter",
                    DisableAifsResetOnSyncfloodCounter, "DramCeccPollingEn",
                    DramCeccPollingEn, "McaPollingEn", McaPollingEn,
                    "PcieAerPollingEn", PcieAerPollingEn, "DramCeccThresholdEn",
                    DramCeccThresholdEn, "McaThresholdEn", McaThresholdEn,
                    "PcieAerThresholdEn", PcieAerThresholdEn,
                    "McaPollingPeriod", McaPollingPeriod,
                    "DramCeccPollingPeriod", DramCeccPollingPeriod,
                    "PcieAerPollingPeriod", PcieAerPollingPeriod,
                    "DramCeccErrThresholdCnt", DramCeccErrThresholdCnt,
                    "McaErrThresholdCnt", McaErrThresholdCnt,
                    "McaUmcErrThresholdCnt", McaUmcErrThresholdCnt,
                    "PcieAerErrThresholdCnt", PcieAerErrThresholdCnt,
                    "Soc0MPList", Soc0MPList, "Soc1MPList", Soc1MPList))
            {
                return;
            }

            if (aifsSignatureIdList)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "AifsSignatureIdList",
                    std::variant<std::map<std::string, std::string>>(
                        *aifsSignatureIdList));
            }

            if (apmlRetries)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "ApmlRetries",
                    std::variant<int64_t>(*apmlRetries));
            }
            if (SystemRecoveryMode)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "SystemRecoveryMode",
                    std::variant<std::string>(*SystemRecoveryMode));
            }
            if (ResetSignalType)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "ResetSignalType",
                    std::variant<std::string>(*ResetSignalType));
            }
            if (HarvestMicrocode)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "HarvestMicrocode",
                    std::variant<bool>(*HarvestMicrocode));
            }
            if (HarvestPPIN)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "HarvestPPIN",
                    std::variant<bool>(*HarvestPPIN));
            }
            if (SigIdOffset)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "SigIdOffset",
                    std::variant<std::vector<std::string>>(*SigIdOffset));
            }
            if (aifsArmed)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "AifsArmed",
                    std::variant<bool>(*aifsArmed));
            }
            if (DisableAifsResetOnSyncfloodCounter)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "DisableAifsResetOnSyncfloodCounter",
                    std::variant<bool>(*DisableAifsResetOnSyncfloodCounter));
            }
            if (DramCeccPollingEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "DramCeccPollingEn",
                    std::variant<bool>(*DramCeccPollingEn));
            }
            if (McaPollingEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "McaPollingEn",
                    std::variant<bool>(*McaPollingEn));
            }
            if (PcieAerPollingEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "PcieAerPollingEn",
                    std::variant<bool>(*PcieAerPollingEn));
            }
            if (DramCeccThresholdEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "DramCeccThresholdEn",
                    std::variant<bool>(*DramCeccThresholdEn));
            }
            if (McaThresholdEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "McaThresholdEn",
                    std::variant<bool>(*McaThresholdEn));
            }
            if (PcieAerThresholdEn)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "PcieAerThresholdEn",
                    std::variant<bool>(*PcieAerThresholdEn));
            }
            if (McaPollingPeriod)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "McaPollingPeriod",
                    std::variant<int64_t>(*McaPollingPeriod));
            }
            if (DramCeccPollingPeriod)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "DramCeccPollingPeriod",
                    std::variant<int64_t>(*DramCeccPollingPeriod));
            }
            if (PcieAerPollingPeriod)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "PcieAerPollingPeriod",
                    std::variant<int64_t>(*PcieAerPollingPeriod));
            }
            if (DramCeccErrThresholdCnt)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "DramCeccErrThresholdCnt",
                    std::variant<int64_t>(*DramCeccErrThresholdCnt));
            }
            if (McaErrThresholdCnt)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "McaErrThresholdCnt",
                    std::variant<int64_t>(*McaErrThresholdCnt));
            }
            if (McaUmcErrThresholdCnt)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "McaUmcErrThresholdCnt",
                    std::variant<int64_t>(*McaUmcErrThresholdCnt));
            }
            if (PcieAerErrThresholdCnt)
            {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "PcieAerErrThresholdCnt",
                    std::variant<int64_t>(*PcieAerErrThresholdCnt));
            }
	        if (Soc0MPList)
            {
                // Update the MPList for socket 0 in mpconfig.json used by AMD-DTS.
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec,
                                uint8_t status) {
                        if (ec || status != 0)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                    },
                    "com.amd.Traces",       // amd-dts service
                    "/com/amd/Traces",       // object path
                    "com.amd.Traces.Tbai",   // interface
                    "UpdateMPConfig",        // method
                    static_cast<uint8_t>(0), // socket 0
                    *Soc0MPList);
                // Update RAS Configuration with new MPList for socket 0
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "Soc0MPList",
                    std::variant<std::vector<std::string>>(*Soc0MPList));
            }
            if (Soc1MPList)
            {
                // Update the MPList for socket 1 in mpconfig.json used by AMD-DTS.
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec,
                                uint8_t status) {
                        if (ec || status != 0)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                    },
                    "com.amd.Traces",
                    "/com/amd/Traces",
                    "com.amd.Traces.Tbai",
                    "UpdateMPConfig",
                    static_cast<uint8_t>(1), // socket 1
                    *Soc1MPList);
                // Update RAS Configuration with new MPList for socket 1
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                        return;
                    },
                    serviceName, "/com/amd/RAS", "com.amd.RAS.Configuration",
                    "SetAttribute", "Soc1MPList",
                    std::variant<std::vector<std::string>>(*Soc1MPList));
            }
        });
}

inline void requestRoutesAmdCrashdumpCollect(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/Crashdump/Actions/LogService.CollectDiagnosticData/")
        // The below is incorrect;  Should be ConfigureManager
        //.privileges(redfish::privileges::postLogService)
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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

                std::string diagnosticDataType;
                std::string oemDiagnosticDataType;
                if (!redfish::json_util::readJsonAction(               //
                        req, asyncResp->res,                           //
                        "DiagnosticDataType", diagnosticDataType,      //
                        "OEMDiagnosticDataType", oemDiagnosticDataType //
                        ))
                {
                    return;
                }

                if (diagnosticDataType != "OEM")
                {
                    BMCWEB_LOG_ERROR(
                        "Only OEM DiagnosticDataType supported for Crashdump");
                    messages::actionParameterValueFormatError(
                        asyncResp->res, diagnosticDataType,
                        "DiagnosticDataType", "CollectDiagnosticData");
                    return;
                }

                OEMDiagnosticType oemDiagType =
                    getOEMDiagnosticType(oemDiagnosticDataType);

                std::string iface;
                std::string method;
                std::string taskMatchStr;
                if (oemDiagType == OEMDiagnosticType::onDemand)
                {
                    iface = crashdumpOnDemandInterface;
                    method = "GenerateOnDemandLog";
                    taskMatchStr =
                        "type='signal',"
                        "interface='org.freedesktop.DBus.Properties',"
                        "member='PropertiesChanged',"
                        "arg0namespace='com.intel.crashdump'";
                }
                else if (oemDiagType == OEMDiagnosticType::telemetry)
                {
                    iface = crashdumpTelemetryInterface;
                    method = "GenerateTelemetryLog";
                    taskMatchStr =
                        "type='signal',"
                        "interface='org.freedesktop.DBus.Properties',"
                        "member='PropertiesChanged',"
                        "arg0namespace='com.intel.crashdump'";
                }
                else
                {
                    BMCWEB_LOG_ERROR("Unsupported OEMDiagnosticDataType: {}",
                                     oemDiagnosticDataType);
                    messages::actionParameterValueFormatError(
                        asyncResp->res, oemDiagnosticDataType,
                        "OEMDiagnosticDataType", "CollectDiagnosticData");
                    return;
                }

                auto collectCrashdumpCallback =
                    [asyncResp, payload(task::Payload(req)),
                     taskMatchStr](const boost::system::error_code& ec,
                                   const std::string&) mutable {
                        if (ec)
                        {
                            if (ec.value() ==
                                boost::system::errc::operation_not_supported)
                            {
                                messages::resourceInStandby(asyncResp->res);
                            }
                            else if (ec.value() == boost::system::errc::
                                                       device_or_resource_busy)
                            {
                                messages::serviceTemporarilyUnavailable(
                                    asyncResp->res, "60");
                            }
                            else
                            {
                                messages::internalError(asyncResp->res);
                            }
                            return;
                        }
                        std::shared_ptr<task::TaskData> task =
                            task::TaskData::createTask(
                                [](const boost::system::error_code& ec2,
                                   sdbusplus::message_t&,
                                   const std::shared_ptr<task::TaskData>&
                                       taskData) {
                                    if (!ec2)
                                    {
                                        taskData->messages.emplace_back(
                                            messages::taskCompletedOK(
                                                std::to_string(
                                                    taskData->index)));
                                        taskData->state = "Completed";
                                    }
                                    return task::completed;
                                },
                                taskMatchStr);

                        task->startTimer(std::chrono::minutes(5));
                        task->populateResp(asyncResp->res);
                        task->payload.emplace(std::move(payload));
                    };

                dbus::utility::async_method_call(
                    asyncResp, std::move(collectCrashdumpCallback),
                    amdCrashdumpObject, amdCrashdumpPath, iface, method);
            });
}

// PPR

inline void requestRoutesPprService(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/")
        .privileges({{"ConfigureManager"}})
        .methods(
            boost::beast::http::verb::
                get)([&app](const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& systemName) {
            if (!redfish::setUpRedfishRoute(app, req, asyncResp))
            {
                return;
            }
            if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
            {
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
                "/redfish/v1/Systems/{}/LogServices/PostPackageRepair",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogService.v1_2_0.LogService";
            asyncResp->res.jsonValue["Name"] = "Open BMC Oem PPR Service";
            asyncResp->res.jsonValue["Description"] =
                "Oem Post Package Repair Service";
            asyncResp->res.jsonValue["Id"] = "ppr";
            asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";
            asyncResp->res.jsonValue["MaxNumberOfRecords"] = 10;
            std::pair<std::string, std::string> redfishDateTimeOffset =
                redfish::time_utils::getDateTimeOffsetNow();
            asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
            asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                redfishDateTimeOffset.second;

            asyncResp->res.jsonValue["Actions"]["#LogService.pprStatus"]
                                    ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/PostPackageRepair/Status",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["Actions"]["#LogService.pprConfig"]
                                    ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/PostPackageRepair/Config",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
            asyncResp->res.jsonValue["Actions"]["#LogService.pprFile"]
                                    ["target"] = std::format(
                "/redfish/v1/Systems/{}/LogServices/PostPackageRepair/RepairData",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
        });
}

// PPR Data

#define MAX_RUNTIME_PPR_CNT (8)
#define PPR_TYPE_BOOTTIME_MASK (0x8000)
#define BT_SET_TO_HARD_MASK 0x0001;
#define RT_TO_BT_MASK 0x0002;
inline bool oobPprEnable = false;

static void setPostPackageRepairData(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp, uint16_t Index,
    uint16_t repairEntryNum, uint16_t repairType, uint16_t socNum,
    std::vector<uint16_t> payload)
{
    std::optional<bool> RecordAdd = true;

    crow::connections::systemBus->async_method_call(
        [asyncResp, Index, RecordAdd, repairEntryNum, repairType, socNum,
         payload](const boost::system::error_code ec1, bool& recordAdd) {
            if (ec1)
            {
                BMCWEB_LOG_ERROR(
                    "DBUS POST Package Repair Record Add error: {} ", ec1);
                messages::internalError(asyncResp->res);
                return;
            }
            BMCWEB_LOG_ERROR("DBUS POST Package Repair Record Add Start {} ",
                             int(recordAdd));

            crow::connections::systemBus->async_method_call(
                [asyncResp, RecordAdd,
                 Index](const boost::system::error_code ec2) {
                    if (ec2)
                    {
                        BMCWEB_LOG_ERROR("D-Bus responses error: {} ", ec2);
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    BMCWEB_LOG_ERROR(
                        "DBUS POST Package Repair Record Add success ");
                    crow::connections::systemBus->async_method_call(
                        [asyncResp, Index](const boost::system::error_code ec3,
                                           const uint32_t& startRuntimeRepair) {
                            if (ec3)
                            {
                                BMCWEB_LOG_ERROR(
                                    "DBUS start Runtime Repair error: {} ",
                                    ec3);
                                messages::internalError(asyncResp->res);
                                return;
                            }
                            BMCWEB_LOG_ERROR(
                                "DBUS success start Runtime Repair : Start {}",
                                startRuntimeRepair);
                        },
                        amdPprFileObject, amdPprFilePath, amdPprFileInterface,
                        "startRuntimeRepair", Index);
                },
                amdPprFileObject, amdPprFilePath, "org.freedesktop.DBus.Properties",
                "Set", amdPprFileInterface, "RecordAdd",
                std::variant<bool>(*RecordAdd));
        },
        amdPprFileObject, amdPprFilePath, amdPprFileInterface,
        "setPostPackageRepairData", repairEntryNum, repairType, socNum,
        payload);
}

inline void requestRoutesPprFile(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/RepairData")
        .privileges(redfish::privileges::patchLogEntry)
        .methods(
            boost::beast::http::verb::
                patch)([&app](
                           const crow::Request& req,
                           const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& systemName) {
            if (!redfish::setUpRedfishRoute(app, req, asyncResp))
            {
                return;
            }
            if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
            {
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

            uint16_t RepairType;
            uint16_t RepairEntryNum;
            uint16_t SocNum;
            uint16_t Index = 0;
            uint16_t RuntimeIndex = 0;
            nlohmann::json jsonRequest;

            if (!json_util::processJsonFromRequest(asyncResp->res, req,
                                                   jsonRequest))
            {
                BMCWEB_LOG_ERROR(
                    "requestRoutesPprFile error in processJsonFromRequest ");
                messages::malformedJSON(asyncResp->res);
                return;
            }

            for (auto& el : jsonRequest["pprDataIn"].items())
            {
                std::vector<uint16_t> Payload;

                if (!json_util::readJson(el.value(), asyncResp->res,
                                         "RepairType", RepairType,
                                         "RepairEntryNum", RepairEntryNum,
                                         "SocNum", SocNum, "Payload", Payload))
                {
                    BMCWEB_LOG_ERROR(
                        "requestRoutesPprFile Error: Issue with Json value read ");
                    messages::malformedJSON(asyncResp->res);
                    return;
                }

                if ((RepairType & PPR_TYPE_BOOTTIME_MASK) == 0)
                {
                    RuntimeIndex++;
                    if (RuntimeIndex > MAX_RUNTIME_PPR_CNT)
                    {
                        BMCWEB_LOG_ERROR(
                            "requestRoutesPprFile Error: Exceed Runtime PPR Max Entry of 8 ");
                        // messages::invalidObject(asyncResp->res);
                        messages::internalError(asyncResp->res);
                        return;
                    }
                }
                setPostPackageRepairData(asyncResp, Index, RepairEntryNum,
                                         RepairType, SocNum, Payload);
                Index++;
            } // end of for loop
            messages::success(asyncResp->res);
        });
}

// PPR Status

inline void requestRoutesPprStatus(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/Status")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
                if (!redfish::setUpRedfishRoute(app, req, asyncResp))
                {
                    return;
                }
                if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
                {
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
                crow::connections::systemBus->async_method_call(
                    [asyncResp](
                        const boost::system::error_code& ec,
                        const std::vector<std::tuple<
                            uint16_t, uint16_t, uint16_t, uint16_t,
                            std::vector<uint16_t>>>& postpackagerepairstatus) {
                        BMCWEB_LOG_ERROR("requestRoutesPprStatus start {}", ec);
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR(
                                "requestRoutesPprStatus got error {}", ec);
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        nlohmann::json pprDataOut = nlohmann::json::array();
                        int count = 0;
                        for (auto resolveList : postpackagerepairstatus)
                        {
                            uint16_t repairEntryNum = std::get<0>(resolveList);
                            uint16_t repairType = std::get<1>(resolveList);
                            uint16_t socNum = std::get<2>(resolveList);
                            uint16_t repairResult = std::get<3>(resolveList);
                            std::vector<uint16_t> payload =
                                std::get<4>(resolveList);

                            nlohmann::json jsonPpr = {
                                {"repairEntryNum", repairEntryNum},
                                {"repairType", repairType},
                                {"socNum", socNum},
                                {"repairResult", repairResult},
                                {"payload", payload}};
                            pprDataOut.push_back(jsonPpr);
                            count++;
                        }

                        asyncResp->res.jsonValue["Members"] = pprDataOut;
                        asyncResp->res.jsonValue["Members@odata.count"] = count;

                        messages::success(asyncResp->res);
                    },
                    amdPprFileObject, amdPprFilePath, amdPprFileInterface,
                    "getPostPackageRepairStatus");
            });
}

// PPR Config

inline void requestRoutesPprGetConfig(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/Config")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
                if (!redfish::setUpRedfishRoute(app, req, asyncResp))
                {
                    return;
                }
                if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
                {
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
                crow::connections::systemBus->async_method_call(
                    [asyncResp](
                        const boost::system::error_code& ec,
                        std::vector<uint16_t>& postpackagerepairconfig) {
                        BMCWEB_LOG_ERROR("requestRoutesGetPprConfig start {}",
                                         ec);
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR(
                                "requestRoutesGetPprConfig got error {}", ec);
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        nlohmann::json pprConfig = nlohmann::json::array();

                        bool RtToBt;
                        bool BtSetToHard;

                        if (postpackagerepairconfig[0] == 0)
                            oobPprEnable = false;
                        else
                            oobPprEnable = true;
                        if (postpackagerepairconfig[1] == 0)
                            RtToBt = false;
                        else
                            RtToBt = true;
                        if (postpackagerepairconfig[2] == 0)
                            BtSetToHard = false;
                        else
                            BtSetToHard = true;

                        nlohmann::json jsonPpr = {
                            {"OobPprEnable", oobPprEnable},
                            {"autoScheduleRtAsBtPpr", RtToBt},
                            {"autoScheduleBtAsHard", BtSetToHard}};
                        pprConfig.push_back(jsonPpr);

                        asyncResp->res.jsonValue["Members"] = pprConfig;
                        asyncResp->res.jsonValue["Members@odata.count"] = 1;

                        messages::success(asyncResp->res);
                    },
                    amdPprFileObject, amdPprFilePath, amdPprFileInterface,
                    "getPostPackageRepairConfig");
            });
}

inline void requestRoutesPprSetConfig(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/Config")
        .privileges(redfish::privileges::patchLogService)
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
                if (!redfish::setUpRedfishRoute(app, req, asyncResp))
                {
                    return;
                }
                if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
                {
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

                std::optional<bool> BtSetToHard;
                std::optional<bool> RtToBt;
                uint16_t flag = 0;
                bool data = false;

                if (!redfish::json_util::readJsonAction(
                        req, asyncResp->res, "autoScheduleBtAsHard",
                        BtSetToHard, "autoScheduleRtAsBtPpr", RtToBt))
                {
                    BMCWEB_LOG_ERROR(
                        "requestRoutesPprSetConfig readJson Error ");
                    return;
                }

                if (BtSetToHard)
                {
                    flag = BT_SET_TO_HARD_MASK;
                    data = BtSetToHard.value();
                }
                if (RtToBt)
                {
                    flag = RT_TO_BT_MASK;
                    data = RtToBt.value();
                }
                if (flag == 0)
                {
                    BMCWEB_LOG_ERROR(
                        "requestRoutesPprSetConfig readJson Flag is 0 ");
                    return;
                }

                crow::connections::systemBus->async_method_call(
                    [asyncResp, flag,
                     data](const boost::system::error_code& ec, bool& result) {
                        BMCWEB_LOG_ERROR("requestRoutesPprSetConfig start {}",
                                         ec);
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR(
                                "requestRoutesPprSetConfig got error {}", ec);
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        BMCWEB_LOG_ERROR(
                            "requestRoutesPprSetConfig end Result {}",
                            int(result));
                        messages::success(asyncResp->res);
                    },
                    amdPprFileObject, amdPprFilePath, amdPprFileInterface,
                    "setPostPackageRepairConfig", flag, data);
            });
}
inline void requestRoutesTraceLogsService(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/TraceLogs/")
        // This is incorrect, should be:
        //.privileges(redfish::privileges::getLogService)
        .privileges({{"ConfigureManager"}})
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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

        // Copy over the static data to include the entries added by
        // SubRoute
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/TraceLogs",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["Name"] = "AMD OEM Tracelog Service";
        asyncResp->res.jsonValue["Description"] =
            "AMD OEM Tracelog service for DTB data";
        asyncResp->res.jsonValue["Id"] = "Tracelogs";
        asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";
        asyncResp->res.jsonValue["MaxNumberOfRecords"] = 53;

        std::pair<std::string, std::string> redfishDateTimeOffset =
            redfish::time_utils::getDateTimeOffsetNow();
        asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
        asyncResp->res.jsonValue["DateTimeLocalOffset"] =
            redfishDateTimeOffset.second;

        asyncResp->res.jsonValue["Entries"]["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/TraceLogs/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/TraceLogs/Actions/LogService.ClearLog",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.CollectDiagnosticData"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/TraceLogs/Actions/LogService.CollectDiagnosticData",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]
                                ["#LogService.CollectDiagnosticData"]
                                ["DiagnosticDataType@Redfish.AllowableValues"] =
            nlohmann::json::array_t({"OEM"});

        crow::connections::systemBus->async_method_call(
            [asyncResp](
                const boost::system::error_code& getSupportedTypesEc,
                const std::vector<std::string>& supportedOemDiagnosticDataTypes) {
                if (getSupportedTypesEc)
                {
                    BMCWEB_LOG_ERROR(
                        "GetSupportedOemDiagnosticDataTypes failed: {}",
                        getSupportedTypesEc.message());
                    return;
                }

                asyncResp->res.jsonValue["Actions"]
                                        ["#LogService.CollectDiagnosticData"]
                                        ["OEMDiagnosticDataType@Redfish.AllowableValues"] =
                    supportedOemDiagnosticDataTypes;
            },
            "com.amd.Traces",
            "/com/amd/Traces",
            "com.amd.Traces.Tbai",
            "GetSupportedOemDiagnosticDataTypes");
    });
}

inline void requestRoutesTraceLogsEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/TraceLogs/Entries/")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName)
    {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        if constexpr (BMCWEB_EXPERIMENTAL_REDFISH_MULTI_COMPUTER_SYSTEM)
        {
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
        uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
            return;
        }

        // Correct DBus interface and path
        constexpr std::array<std::string_view, 1> interfaces = {
            "com.amd.crashdump"};

        dbus::utility::getSubTreePaths(
            amdTracelogPath, 0, interfaces,
            [asyncResp, hostNumber](
                const boost::system::error_code& ec,
                const std::vector<std::string>& resp)
        {
            if (ec)
            {
                if (ec.value() !=
                    boost::system::errc::no_such_file_or_directory)
                {
                    BMCWEB_LOG_ERROR(
                        "Failed to get trace entries: {}",
                        ec.message());
                    messages::internalError(asyncResp->res);
                    return;
                }
            }

            // Redfish response base
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntryCollection.LogEntryCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/TraceLogs/Entries";
            asyncResp->res.jsonValue["Name"] =
                "OpenBMC TraceLog Entries";
            asyncResp->res.jsonValue["Description"] =
                "Collection of TraceLog Entries";
            asyncResp->res.jsonValue["Members"] =
                nlohmann::json::array();

            size_t count = 0;
            for (const std::string& path : resp)
            {
                const sdbusplus::message::object_path objPath(path);

                std::string logID = objPath.filename();
                if (logID.empty())
                {
                    continue;
                }
                // Reuse existing helper
                logTraceLogEntry(asyncResp,
                                 logID,
                                 hostNumber,
                                 asyncResp->res.jsonValue["Members"]);

                count++;
            }

            asyncResp->res.jsonValue["Members@odata.count"] = count;
        });
    });
}

inline void requestRoutesTraceLogCollect(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/TraceLogs/Actions/LogService.CollectDiagnosticData/")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName)
    {
        // Standard Redfish setup
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        // Validate system
        if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        // Host validation
        uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
            return;
        }

        std::optional<std::string> diagnosticDataType;
        std::optional<std::string> oemDiagnosticDataType;
        if (!redfish::json_util::readJsonAction(
                req, asyncResp->res, "DiagnosticDataType", diagnosticDataType,
                "OEMDiagnosticDataType", oemDiagnosticDataType))
        {
            return;
        }

        if (!diagnosticDataType)
        {
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData", "DiagnosticDataType");
            return;
        }

        if (*diagnosticDataType != "OEM")
        {
            messages::actionParameterValueNotInList(
                asyncResp->res, *diagnosticDataType, "DiagnosticDataType",
                "CollectDiagnosticData");
            return;
        }

        if (!oemDiagnosticDataType)
        {
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData",
                "OEMDiagnosticDataType");
            return;
        }

        task::Payload payload(req);
        std::string requestedOemDiagnosticDataType =
            std::move(*oemDiagnosticDataType);

        crow::connections::systemBus->async_method_call(
            [asyncResp, payload = std::move(payload),
             requestedOemDiagnosticDataType =
                 std::move(requestedOemDiagnosticDataType)](
                const boost::system::error_code& getSupportedTypesEc,
                const std::vector<std::string>& supportedOemDiagnosticDataTypes)
                mutable {
                if (getSupportedTypesEc)
                {
                    BMCWEB_LOG_ERROR(
                        "GetSupportedOemDiagnosticDataTypes failed: {}",
                        getSupportedTypesEc.message());
                    messages::internalError(asyncResp->res);
                    return;
                }

                if (std::ranges::find(supportedOemDiagnosticDataTypes,
                                      requestedOemDiagnosticDataType) ==
                    supportedOemDiagnosticDataTypes.end())
                {
                    messages::actionParameterValueNotInList(
                        asyncResp->res, requestedOemDiagnosticDataType,
                        "OEMDiagnosticDataType", "CollectDiagnosticData");
                    return;
                }

        std::vector<std::pair<std::string,
            std::variant<std::string, uint64_t>>> createDumpParamVec;

        createDumpParamVec.emplace_back(
            "xyz.openbmc_project.Dump.Create.CreateParameters.DumpType",
            std::string("UserRequested"));

        createDumpParamVec.emplace_back(
            "com.amd.Dump.Create.CreateParameters.MPName",
            requestedOemDiagnosticDataType);

        // Create async task
        std::shared_ptr<task::TaskData> task;

        try
        {
            task = task::TaskData::createTask(
                [](boost::system::error_code ec,
                   sdbusplus::message::message& msg,
                   const std::shared_ptr<task::TaskData>& taskPtr) -> bool
                {
                    //  Handle DBus error
                    if (ec)
                    {
                        BMCWEB_LOG_ERROR("Task error: {}", ec.message());
                        taskPtr->state = "Exception";
                        taskPtr->messages.emplace_back(
                            "Trace collection failed: " + ec.message());
                        return false;
                    }

                    // Prevent duplicate completion
                    if (taskPtr->state == "Completed")
                    {
                        return false;
                    }

                    std::string filePath;

                    try
                    {
                        msg.read(filePath);

                        // Validate data
                        if (filePath.empty())
                        {
                            BMCWEB_LOG_ERROR("Empty file path received");
                            taskPtr->state = "Exception";
                            return false;
                        }

                        BMCWEB_LOG_DEBUG(
                            "Received OnDemandMPDataAvailable: {}",
                            filePath);

                        taskPtr->messages.emplace_back(
                            "Trace collected: " + filePath);
                    }
                    catch (const std::exception& e)
                    {
                        BMCWEB_LOG_ERROR(
                            "Failed to read DBus signal: {}", e.what());

                        taskPtr->state = "Exception";
                        taskPtr->messages.emplace_back(
                            "Trace collection failed: " +
                            std::string(e.what()));
                        return false;
                    }

                    // Mark task completed
                    taskPtr->state = "Completed";
                    taskPtr->percentComplete = 100;

                    return true;
                },
                // Match custom signal
                "type='signal',"
                "path='/com/amd/Traces',"
                "interface='com.amd.Traces.Signal',"
                "member='OnDemandMPDataAvailable'");
        }
        catch (const sdbusplus::exception::SdBusError& e)
        {
            BMCWEB_LOG_ERROR("DBus exception: {}", e.what());
            messages::internalError(asyncResp->res);
            return;
        }

        //  Start task
        task->startTimer(std::chrono::minutes(5));
        task->populateResp(asyncResp->res);
        task->payload.emplace(std::move(payload));
        task->state = "Running";

        asyncResp->res.jsonValue["TaskID"] =
            std::to_string(task->index);

        asyncResp->res.result(boost::beast::http::status::accepted);
        asyncResp->res.end();

        // Trigger DBus CreateDump
        crow::connections::systemBus->async_method_call(
            [task](const boost::system::error_code ec)
            {
                if (ec)
                {
                    BMCWEB_LOG_ERROR("CreateDump failed: {}",
                                     ec.message());
                    task->state = "Exception";
                    task->messages.emplace_back(
                        "Failed to trigger trace collection");
                }
            },
            "com.amd.Traces",      // service
            "/com/amd/Traces",     // object path
            "xyz.openbmc_project.Dump.Create", // interface
            "CreateDump",                      // method
            createDumpParamVec);
            },
            "com.amd.Traces",
            "/com/amd/Traces",
            "com.amd.Traces.Tbai",
            "GetSupportedOemDiagnosticDataTypes");
    });
}

inline void requestRoutesTraceLogsFile(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/TraceLogs/Entries/<str>/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& systemName,
               const std::string& logID,
               const std::string& fileName)
    {
        if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        uint8_t hostNumber = http_helpers::getHostNumberFromUrl(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
            return;
        }

        // Trace dump objects live under com.amd.Traces; HostNumber is
        // accepted for URI compatibility with Crashdump.
        std::string service = "com.amd.Traces";

        constexpr std::string_view TracedumpInterface =
            "com.amd.crashdump";

        auto callback =
            [asyncResp, logID, fileName](
                const boost::system::error_code& ec,
                const std::vector<
                    std::pair<std::string, dbus::utility::DbusVariantType>>& resp)
        {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBus error: {}", ec.message());
                messages::internalError(asyncResp->res);
                return;
            }

            std::string dbusFilename;
            std::string dbusTimestamp;
            std::string dbusFilepath;

            parseCrashdumpParameters(resp, dbusFilename,
                                     dbusTimestamp, dbusFilepath);

            //  Validate
            if (dbusFilename.empty() || dbusFilepath.empty())
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            // Match filename
            if (fileName != dbusFilename)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            // Open file
            if (asyncResp->res.openFile(dbusFilepath) !=
                crow::OpenCode::Success)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            // Force download
            asyncResp->res.addHeader(
                boost::beast::http::field::content_disposition,
                "attachment; filename=\"" + dbusFilename + "\"");
        };

        sdbusplus::asio::getAllProperties(
            *crow::connections::systemBus,
            service,
            std::string(amdTracelogPath) + "/" + logID,
            std::string(TracedumpInterface),
            std::move(callback));
    });
}

} // namespace redfish
