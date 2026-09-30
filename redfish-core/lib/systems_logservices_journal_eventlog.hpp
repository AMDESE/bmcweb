// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
// SPDX-FileCopyrightText: Copyright 2018 Intel Corporation
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "async_resp.hpp"
#include "error_messages.hpp"
#include "http_request.hpp"
#include "logging.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "utils/eventlog_utils.hpp"
#include "utils/query_param.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/linux_error.hpp>
#include <boost/url/format.hpp>
#include <boost/url/url.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <sdbusplus/unpack_properties.hpp>
#include <systemd/sd-journal.h>

#include <map>
#include <string>

namespace redfish
{

inline void handleSystemsLogServiceEventLogLogEntryCollection(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName)
{
    query_param::QueryCapabilities capabilities = {
        .canDelegateTop = true,
        .canDelegateSkip = true,
    };
    query_param::Query delegatedQuery;
    if (!redfish::setUpRedfishRouteWithDelegation(app, req, asyncResp,
                                                  delegatedQuery, capabilities))
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

    eventlog_utils::
        handleSystemsAndManagersLogServiceEventLogLogEntryCollection(
            asyncResp, delegatedQuery,
            eventlog_utils::LogServiceParentCollection::Systems);
}

inline void handleSystemsLogServiceEventLogEntriesGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& param)
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

    eventlog_utils::handleSystemsAndManagersLogServiceEventLogEntriesGet(
        asyncResp, param, eventlog_utils::LogServiceParentCollection::Systems);
}

inline void handleSystemsLogServicesEventLogActionsClearPost(
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

    eventlog_utils::handleSystemsAndManagersLogServicesEventLogActionsClearPost(
        asyncResp);
}

inline void handleSystemsJournalEventLogEntryPost(
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
    if (!eventlog_utils::isUsbVnicEventLogClient(req))
    {
        messages::insufficientPrivilege(asyncResp->res);
        return;
    }

    std::string message;
    std::string redfishMessageId = "OpenBMC.0.1.GeneralError";
    std::string redfishMessageArgs = message;
    std::string severity;
    int severityNumber = 0;
    std::map<std::string, std::string> additionalData;

    if (!eventlog_utils::parseEventLogCreateBody(
            req, asyncResp, message, redfishMessageId, redfishMessageArgs,
            severity, severityNumber, additionalData))
    {
        BMCWEB_LOG_ERROR("Error in parsing the request body");
        return;
    }

    sd_journal_send("MESSAGE=%s", message.c_str(), "PRIORITY=%i",
                    severityNumber, "REDFISH_MESSAGE_ID=%s",
                    redfishMessageId.c_str(), "REDFISH_MESSAGE_ARGS=%s",
                    redfishMessageArgs.c_str(), nullptr);

    std::string mesg = "MESSAGE=" + message + ", PRIORITY=" +
                       std::to_string(severityNumber) + ", SEVERITY=" +
                       severity + ", REDFISH_MESSAGE_ID=" + redfishMessageId +
                       ", REDFISH_MESSAGE_ARGS=" + redfishMessageArgs;
    for (const auto& [key, value] : additionalData)
    {
        mesg += ", " + key + "=" + value;
    }
    BMCWEB_LOG_ERROR("Redfish Event logged; EVENT: {}", mesg);

    messages::success(asyncResp->res);
}

inline void requestRoutesSystemsJournalEventLog(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleSystemsLogServiceEventLogLogEntryCollection, std::ref(app)));

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleSystemsLogServiceEventLogEntriesGet, std::ref(app)));

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/EventLog/Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::
                        postLogServiceSubOverComputerSystemLogServiceCollection)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleSystemsLogServicesEventLogActionsClearPost, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog/")
        .privileges(redfish::privileges::postLogEntry)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleSystemsJournalEventLogEntryPost, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog")
        .privileges(redfish::privileges::postLogEntry)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleSystemsJournalEventLogEntryPost, std::ref(app)));
}
} // namespace redfish
