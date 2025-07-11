/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/update_service.hpp"
#include "multipart_parser.hpp"
#include "ossl_random.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "task.hpp"
#include "task_messages.hpp"
#include "utils/collection.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/sw_utils.hpp"

#include <sys/mman.h>

#include <boost/system/error_code.hpp>
#include <boost/url/format.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/unpack_properties.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace redfish
{

// Match signals added on software path
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<sdbusplus::bus::match_t> fwUpdateMatcher;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<sdbusplus::bus::match_t> fwUpdateErrorMatcher;
// Only allow one update at a time
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static bool fwUpdateInProgress = false;
// Timer for software available
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<boost::asio::steady_timer> fwAvailableTimer;

struct MemoryFileDescriptor
{
    int fd = -1;

    explicit MemoryFileDescriptor(const std::string& filename) :
        fd(memfd_create(filename.c_str(), 0))
    {}

    MemoryFileDescriptor(const MemoryFileDescriptor&) = default;
    MemoryFileDescriptor(MemoryFileDescriptor&& other) noexcept : fd(other.fd)
    {
        other.fd = -1;
    }
    MemoryFileDescriptor& operator=(const MemoryFileDescriptor&) = delete;
    MemoryFileDescriptor& operator=(MemoryFileDescriptor&&) = default;

    ~MemoryFileDescriptor()
    {
        if (fd != -1)
        {
            close(fd);
        }
    }

    bool rewind() const
    {
        if (lseek(fd, 0, SEEK_SET) == -1)
        {
            BMCWEB_LOG_ERROR("Failed to seek to beginning of image memfd");
            return false;
        }
        return true;
    }
};

inline void cleanUp()
{
    fwUpdateInProgress = false;
    fwUpdateMatcher = nullptr;
    fwUpdateErrorMatcher = nullptr;
}

inline void activateImage(const std::string& objPath,
                          const std::string& service, uint16_t hostNumber)
{
    BMCWEB_LOG_DEBUG("Activate image for {} {}", objPath, service);
    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Software.Activation", "RequestedActivation",
        "xyz.openbmc_project.Software.Activation.RequestedActivations.Active",
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("error_code = {}", ec);
            BMCWEB_LOG_DEBUG("error msg = {}", ec.message());
        }
    });

    sdbusplus::asio::setProperty(
        *crow::connections::systemBus, service, objPath,
        "xyz.openbmc_project.Software.Activation", "HostNumber", hostNumber,
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            BMCWEB_LOG_CRITICAL("error_code = {}", ec);
            BMCWEB_LOG_CRITICAL("error msg = {}", ec.message());
        }
    });
}

inline bool handleCreateTask(const boost::system::error_code& ec2,
                             sdbusplus::message_t& msg,
                             const std::shared_ptr<task::TaskData>& taskData)
{
    if (ec2)
    {
        return task::completed;
    }

    std::string iface;
    dbus::utility::DBusPropertiesMap values;

    std::string index = std::to_string(taskData->index);
    msg.read(iface, values);

    if (iface == "xyz.openbmc_project.Software.Activation")
    {
        const std::string* state = nullptr;
        for (const auto& property : values)
        {
            if (property.first == "Activation")
            {
                state = std::get_if<std::string>(&property.second);
                if (state == nullptr)
                {
                    taskData->messages.emplace_back(messages::internalError());
                    return task::completed;
                }
            }
        }

        if (state == nullptr)
        {
            return !task::completed;
        }

        if (state->ends_with("Invalid") || state->ends_with("Failed"))
        {
            taskData->state = "Exception";
            taskData->status = "Warning";
            taskData->messages.emplace_back(messages::taskAborted(index));
            return task::completed;
        }

        if (state->ends_with("Staged"))
        {
            taskData->state = "Stopping";
            taskData->messages.emplace_back(messages::taskPaused(index));

            // its staged, set a long timer to
            // allow them time to complete the
            // update (probably cycle the
            // system) if this expires then
            // task will be canceled
            taskData->extendTimer(std::chrono::hours(5));
            return !task::completed;
        }

        if (state->ends_with("Active"))
        {
            taskData->messages.emplace_back(messages::taskCompletedOK(index));
            taskData->state = "Completed";
            return task::completed;
        }
    }
    else if (iface == "xyz.openbmc_project.Software.ActivationProgress")
    {
        const uint8_t* progress = nullptr;
        for (const auto& property : values)
        {
            if (property.first == "Progress")
            {
                progress = std::get_if<uint8_t>(&property.second);
                if (progress == nullptr)
                {
                    taskData->messages.emplace_back(messages::internalError());
                    return task::completed;
                }
            }
        }

        if (progress == nullptr)
        {
            return !task::completed;
        }
        taskData->percentComplete = *progress;
        taskData->messages.emplace_back(
            messages::taskProgressChanged(index, *progress));

        // if we're getting status updates it's
        // still alive, update timer
        taskData->extendTimer(std::chrono::minutes(5));
    }

    // as firmware update often results in a
    // reboot, the task  may never "complete"
    // unless it is an error

    return !task::completed;
}

inline void createTask(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       task::Payload&& payload,
                       const sdbusplus::message::object_path& objPath)
{
    std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
        std::bind_front(handleCreateTask),
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='" +
            objPath.str + "'");
    task->startTimer(std::chrono::minutes(5));
    task->populateResp(asyncResp->res);
    task->payload.emplace(std::move(payload));
}

// Note that asyncResp can be either a valid pointer or nullptr. If nullptr
// then no asyncResp updates will occur
static void
    softwareInterfaceAdded(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           sdbusplus::message_t& m, task::Payload&& payload,
                           uint16_t hostNumber)
{
    dbus::utility::DBusInterfacesMap interfacesProperties;

    sdbusplus::message::object_path objPath;

    m.read(objPath, interfacesProperties);

    BMCWEB_LOG_DEBUG("obj path = {}", objPath.str);
    for (const auto& interface : interfacesProperties)
    {
        BMCWEB_LOG_DEBUG("interface = {}", interface.first);

        if (interface.first == "xyz.openbmc_project.Software.Activation")
        {
            // Retrieve service and activate
            constexpr std::array<std::string_view, 1> interfaces = {
                "xyz.openbmc_project.Software.Activation"};
            dbus::utility::getDbusObject(
                objPath.str, interfaces,
                [objPath, asyncResp, hostNumber, payload(std::move(payload))](
                    const boost::system::error_code& ec,
                    const std::vector<
                        std::pair<std::string, std::vector<std::string>>>&
                        objInfo) mutable {
                if (ec)
                {
                    BMCWEB_LOG_DEBUG("error_code = {}", ec);
                    BMCWEB_LOG_DEBUG("error msg = {}", ec.message());
                    if (asyncResp)
                    {
                        messages::internalError(asyncResp->res);
                    }
                    cleanUp();
                    return;
                }
                // Ensure we only got one service back
                if (objInfo.size() != 1)
                {
                    BMCWEB_LOG_ERROR("Invalid Object Size {}", objInfo.size());
                    if (asyncResp)
                    {
                        messages::internalError(asyncResp->res);
                    }
                    cleanUp();
                    return;
                }
                // cancel timer only when
                // xyz.openbmc_project.Software.Activation interface
                // is added
                fwAvailableTimer = nullptr;
                activateImage(objPath.str, objInfo[0].first, hostNumber);
                if (asyncResp)
                {
                    createTask(asyncResp, std::move(payload), objPath);
                }
                fwUpdateInProgress = false;
            });

            break;
        }
    }
}

inline void afterAvailbleTimerAsyncWait(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::system::error_code& ec)
{
    cleanUp();
    if (ec == boost::asio::error::operation_aborted)
    {
        // expected, we were canceled before the timer completed.
        return;
    }
    BMCWEB_LOG_ERROR("Timed out waiting for firmware object being created");
    BMCWEB_LOG_ERROR("FW image may has already been uploaded to server");
    if (ec)
    {
        BMCWEB_LOG_ERROR("Async_wait failed{}", ec);
        return;
    }
    if (asyncResp)
    {
        redfish::messages::internalError(asyncResp->res);
    }
}

inline void
    handleUpdateErrorType(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& url, const std::string& type)
{
    if (type == "xyz.openbmc_project.Software.Image.Error.UnTarFailure")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Invalid archive");
    }
    else if (type ==
             "xyz.openbmc_project.Software.Image.Error.ManifestFileFailure")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Invalid manifest");
    }
    else if (type == "xyz.openbmc_project.Software.Image.Error.ImageFailure")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Invalid image format");
    }
    else if (type == "xyz.openbmc_project.Software.Version.Error.AlreadyExists")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Image version already exists");

        redfish::messages::resourceAlreadyExists(
            asyncResp->res, "UpdateService", "Version", "uploaded version");
    }
    else if (type == "xyz.openbmc_project.Software.Image.Error.BusyFailure")
    {
        redfish::messages::resourceExhaustion(asyncResp->res, url);
    }
    else if (type == "xyz.openbmc_project.Software.Version.Error.Incompatible")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Incompatible image version");
    }
    else if (type ==
             "xyz.openbmc_project.Software.Version.Error.ExpiredAccessKey")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Update Access Key Expired");
    }
    else if (type ==
             "xyz.openbmc_project.Software.Version.Error.InvalidSignature")
    {
        redfish::messages::invalidUpload(asyncResp->res, url,
                                         "Invalid image signature");
    }
    else if (type ==
                 "xyz.openbmc_project.Software.Image.Error.InternalFailure" ||
             type == "xyz.openbmc_project.Software.Version.Error.HostFile")
    {
        BMCWEB_LOG_ERROR("Software Image Error type={}", type);
        redfish::messages::internalError(asyncResp->res);
    }
    else
    {
        // Unrelated error types. Ignored
        BMCWEB_LOG_INFO("Non-Software-related Error type={}. Ignored", type);
        return;
    }
    // Clear the timer
    fwAvailableTimer = nullptr;
}

inline void
    afterUpdateErrorMatcher(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& url, sdbusplus::message_t& m)
{
    dbus::utility::DBusInterfacesMap interfacesProperties;
    sdbusplus::message::object_path objPath;
    m.read(objPath, interfacesProperties);
    BMCWEB_LOG_DEBUG("obj path = {}", objPath.str);
    for (const std::pair<std::string, dbus::utility::DBusPropertiesMap>&
             interface : interfacesProperties)
    {
        if (interface.first == "xyz.openbmc_project.Logging.Entry")
        {
            for (const std::pair<std::string, dbus::utility::DbusVariantType>&
                     value : interface.second)
            {
                if (value.first != "Message")
                {
                    continue;
                }
                const std::string* type =
                    std::get_if<std::string>(&value.second);
                if (type == nullptr)
                {
                    // if this was our message, timeout will cover it
                    return;
                }
                handleUpdateErrorType(asyncResp, url, *type);
            }
        }
    }
}

// Note that asyncResp can be either a valid pointer or nullptr. If nullptr
// then no asyncResp updates will occur
inline void monitorForSoftwareAvailable(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const crow::Request& req, const std::string& url,
    int timeoutTimeSeconds = 25)
{
    // Only allow one FW update at a time
    if (fwUpdateInProgress)
    {
        if (asyncResp)
        {
            messages::serviceTemporarilyUnavailable(asyncResp->res, "30");
        }
        return;
    }

    if (req.ioService == nullptr)
    {
        messages::internalError(asyncResp->res);
        return;
    }

    boost::urls::url_view urlView = req.url();
    uint16_t hostNumber;

    for (const auto& param : urlView.params())
    {
        if (param.key == "HostNumber" && !param.value.empty())
        {
            try
            {
                int temp = std::stoi(std::string(param.value));
                hostNumber = static_cast<uint16_t>(temp);
            }
            catch (const std::exception& e)
            {
                BMCWEB_LOG_WARNING("Invalid HostNumber format: {}",
                                   param.value);
                hostNumber = 0;
            }
            break;
        }
    }

    if (hostNumber > 2)
    {
        messages::actionParameterNotSupported(
            asyncResp->res, std::to_string(hostNumber), "HostNumber");
    }

    fwAvailableTimer =
        std::make_unique<boost::asio::steady_timer>(*req.ioService);

    fwAvailableTimer->expires_after(std::chrono::seconds(timeoutTimeSeconds));

    fwAvailableTimer->async_wait(
        std::bind_front(afterAvailbleTimerAsyncWait, asyncResp));

    task::Payload payload(req);
    auto callback = [asyncResp, hostNumber,
                     payload](sdbusplus::message_t& m) mutable {
        BMCWEB_LOG_DEBUG("Match fired");
        softwareInterfaceAdded(asyncResp, m, std::move(payload), hostNumber);
    };

    fwUpdateInProgress = true;

    fwUpdateMatcher = std::make_unique<sdbusplus::bus::match_t>(
        *crow::connections::systemBus,
        "interface='org.freedesktop.DBus.ObjectManager',type='signal',"
        "member='InterfacesAdded',path='/xyz/openbmc_project/software'",
        callback);

    fwUpdateErrorMatcher = std::make_unique<sdbusplus::bus::match_t>(
        *crow::connections::systemBus,
        "interface='org.freedesktop.DBus.ObjectManager',type='signal',"
        "member='InterfacesAdded',"
        "path='/xyz/openbmc_project/logging'",
        std::bind_front(afterUpdateErrorMatcher, asyncResp, url));
}

inline std::optional<boost::urls::url>
    parseSimpleUpdateUrl(std::string imageURI,
                         std::optional<std::string> transferProtocol,
                         crow::Response& res)
{
    if (imageURI.find("://") == std::string::npos)
    {
        if (imageURI.starts_with("/"))
        {
            messages::actionParameterValueTypeError(
                res, imageURI, "ImageURI", "UpdateService.SimpleUpdate");
            return std::nullopt;
        }
        if (!transferProtocol)
        {
            messages::actionParameterValueTypeError(
                res, imageURI, "ImageURI", "UpdateService.SimpleUpdate");
            return std::nullopt;
        }
        // OpenBMC currently only supports TFTP or HTTPS
        if (*transferProtocol == "TFTP")
        {
            imageURI = "tftp://" + imageURI;
        }
        else if (*transferProtocol == "HTTPS")
        {
            imageURI = "https://" + imageURI;
        }
        else
        {
            messages::actionParameterNotSupported(res, "TransferProtocol",
                                                  *transferProtocol);
            BMCWEB_LOG_ERROR("Request incorrect protocol parameter: {}",
                             *transferProtocol);
            return std::nullopt;
        }
    }

    boost::system::result<boost::urls::url> url =
        boost::urls::parse_absolute_uri(imageURI);
    if (!url)
    {
        messages::actionParameterValueTypeError(res, imageURI, "ImageURI",
                                                "UpdateService.SimpleUpdate");

        return std::nullopt;
    }
    url->normalize();

    if (url->scheme() == "tftp")
    {
        if (url->encoded_path().size() < 2)
        {
            messages::actionParameterNotSupported(res, "ImageURI",
                                                  url->buffer());
            return std::nullopt;
        }
    }
    else if (url->scheme() == "https")
    {
        // Empty paths default to "/"
        if (url->encoded_path().empty())
        {
            url->set_encoded_path("/");
        }
    }
    else
    {
        messages::actionParameterNotSupported(res, "ImageURI", imageURI);
        return std::nullopt;
    }

    if (url->encoded_path().empty())
    {
        messages::actionParameterValueTypeError(res, imageURI, "ImageURI",
                                                "UpdateService.SimpleUpdate");
        return std::nullopt;
    }

    return *url;
}

inline void doHttpsUpdate(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const boost::urls::url_view_base& url)
{
    messages::actionParameterNotSupported(asyncResp->res, "ImageURI",
                                          url.buffer());
}

inline void doTftpUpdate(const crow::Request& req,
                         const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const boost::urls::url_view_base& url)
{
    if (!BMCWEB_INSECURE_TFTP_UPDATE)
    {
        messages::actionParameterNotSupported(asyncResp->res, "ImageURI",
                                              url.buffer());
        return;
    }

    std::string path(url.encoded_path());
    if (path.size() < 2)
    {
        messages::actionParameterNotSupported(asyncResp->res, "ImageURI",
                                              url.buffer());
        return;
    }
    // TFTP expects a path without a /
    path.erase(0, 1);
    std::string host(url.encoded_host_and_port());
    BMCWEB_LOG_DEBUG("Server: {} File: {}", host, path);

    // Setup callback for when new software detected
    // Give TFTP 10 minutes to complete
    monitorForSoftwareAvailable(
        asyncResp, req,
        "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate", 600);

    // TFTP can take up to 10 minutes depending on image size and
    // connection speed. Return to caller as soon as the TFTP operation
    // has been started. The callback above will ensure the activate
    // is started once the download has completed
    redfish::messages::success(asyncResp->res);

    // Call TFTP service
    crow::connections::systemBus->async_method_call(
        [](const boost::system::error_code& ec) {
        if (ec)
        {
            // messages::internalError(asyncResp->res);
            cleanUp();
            BMCWEB_LOG_DEBUG("error_code = {}", ec);
            BMCWEB_LOG_DEBUG("error msg = {}", ec.message());
        }
        else
        {
            BMCWEB_LOG_DEBUG("Call to DownloaViaTFTP Success");
        }
    }, "xyz.openbmc_project.Software.Download", "/xyz/openbmc_project/software",
        "xyz.openbmc_project.Common.TFTP", "DownloadViaTFTP", path, host);
}

inline void handleUpdateServiceSimpleUpdateAction(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    std::optional<std::string> transferProtocol;
    std::string imageURI;

    BMCWEB_LOG_DEBUG("Enter UpdateService.SimpleUpdate doPost");

    // User can pass in both TransferProtocol and ImageURI parameters or
    // they can pass in just the ImageURI with the transfer protocol
    // embedded within it.
    // 1) TransferProtocol:TFTP ImageURI:1.1.1.1/myfile.bin
    // 2) ImageURI:tftp://1.1.1.1/myfile.bin

    if (!json_util::readJsonAction(req, asyncResp->res, "TransferProtocol",
                                   transferProtocol, "ImageURI", imageURI))
    {
        BMCWEB_LOG_DEBUG("Missing TransferProtocol or ImageURI parameter");
        return;
    }

    std::optional<boost::urls::url> url =
        parseSimpleUpdateUrl(imageURI, transferProtocol, asyncResp->res);
    if (!url)
    {
        return;
    }
    if (url->scheme() == "tftp")
    {
        doTftpUpdate(req, asyncResp, *url);
    }
    else if (url->scheme() == "https")
    {
        doHttpsUpdate(asyncResp, *url);
    }
    else
    {
        messages::actionParameterNotSupported(asyncResp->res, "ImageURI",
                                              url->buffer());
        return;
    }

    BMCWEB_LOG_DEBUG("Exit UpdateService.SimpleUpdate doPost");
}

inline void uploadImageFile(crow::Response& res, std::string_view body)
{
    std::filesystem::path filepath("/tmp/images/" + bmcweb::getRandomUUID());

    BMCWEB_LOG_DEBUG("Writing file to {}", filepath.string());
    std::ofstream out(filepath, std::ofstream::out | std::ofstream::binary |
                                    std::ofstream::trunc);
    // set the permission of the file to 640
    std::filesystem::perms permission = std::filesystem::perms::owner_read |
                                        std::filesystem::perms::group_read;
    std::filesystem::permissions(filepath, permission);
    out << body;

    if (out.bad())
    {
        messages::internalError(res);
        cleanUp();
    }
}

// Convert the Request Apply Time to the D-Bus value
inline bool convertApplyTime(crow::Response& res, const std::string& applyTime,
                             std::string& applyTimeNewVal)
{
    if (applyTime == "Immediate")
    {
        applyTimeNewVal =
            "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate";
    }
    else if (applyTime == "OnReset")
    {
        applyTimeNewVal =
            "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.OnReset";
    }
    else
    {
        BMCWEB_LOG_WARNING(
            "ApplyTime value {} is not in the list of acceptable values",
            applyTime);
        messages::propertyValueNotInList(res, applyTime, "ApplyTime");
        return false;
    }
    return true;
}

inline void setApplyTime(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const std::string& applyTime)
{
    std::string applyTimeNewVal;
    if (!convertApplyTime(asyncResp->res, applyTime, applyTimeNewVal))
    {
        return;
    }

    setDbusProperty(asyncResp, "ApplyTime", "xyz.openbmc_project.Settings",
                    sdbusplus::message::object_path(
                        "/xyz/openbmc_project/software/apply_time"),
                    "xyz.openbmc_project.Software.ApplyTime",
                    "RequestedApplyTime", applyTimeNewVal);
}

struct MultiPartUpdateParameters
{
    std::optional<std::string> applyTime;
    std::string uploadData;
    std::vector<std::string> targets;
};

inline std::optional<std::string>
    processUrl(boost::system::result<boost::urls::url_view>& url)
{
    if (!url)
    {
        return std::nullopt;
    }
    if (crow::utility::readUrlSegments(*url, "redfish", "v1", "Managers",
                                       BMCWEB_REDFISH_MANAGER_URI_NAME))
    {
        return std::make_optional(std::string(BMCWEB_REDFISH_MANAGER_URI_NAME));
    }
    if constexpr (!BMCWEB_REDFISH_UPDATESERVICE_USE_DBUS)
    {
        return std::nullopt;
    }
    std::string firmwareId;
    if (!crow::utility::readUrlSegments(*url, "redfish", "v1", "UpdateService",
                                        "FirmwareInventory",
                                        std::ref(firmwareId)))
    {
        return std::nullopt;
    }

    return std::make_optional(firmwareId);
}

inline std::optional<MultiPartUpdateParameters>
    extractMultipartUpdateParameters(
        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
        MultipartParser parser)
{
    MultiPartUpdateParameters multiRet;
    for (FormPart& formpart : parser.mime_fields)
    {
        boost::beast::http::fields::const_iterator it =
            formpart.fields.find("Content-Disposition");
        if (it == formpart.fields.end())
        {
            BMCWEB_LOG_ERROR("Couldn't find Content-Disposition");
            return std::nullopt;
        }
        BMCWEB_LOG_INFO("Parsing value {}", it->value());

        // The construction parameters of param_list must start with `;`
        size_t index = it->value().find(';');
        if (index == std::string::npos)
        {
            continue;
        }

        for (const auto& param :
             boost::beast::http::param_list{it->value().substr(index)})
        {
            if (param.first != "name" || param.second.empty())
            {
                continue;
            }

            if (param.second == "UpdateParameters")
            {
                std::vector<std::string> tempTargets;
                nlohmann::json content =
                    nlohmann::json::parse(formpart.content);
                nlohmann::json::object_t* obj =
                    content.get_ptr<nlohmann::json::object_t*>();
                if (obj == nullptr)
                {
                    messages::propertyValueTypeError(
                        asyncResp->res, formpart.content, "UpdateParameters");
                    return std::nullopt;
                }

                if (!json_util::readJsonObject(
                        *obj, asyncResp->res, "Targets", tempTargets,
                        "@Redfish.OperationApplyTime", multiRet.applyTime))
                {
                    return std::nullopt;
                }

                for (size_t urlIndex = 0; urlIndex < tempTargets.size();
                     urlIndex++)
                {
                    const std::string& target = tempTargets[urlIndex];
                    boost::system::result<boost::urls::url_view> url =
                        boost::urls::parse_origin_form(target);
                    auto res = processUrl(url);
                    if (!res.has_value())
                    {
                        messages::propertyValueFormatError(
                            asyncResp->res, target,
                            std::format("Targets/{}", urlIndex));
                        return std::nullopt;
                    }
                    multiRet.targets.emplace_back(res.value());
                }
                if (multiRet.targets.size() != 1)
                {
                    messages::propertyValueFormatError(
                        asyncResp->res, multiRet.targets, "Targets");
                    return std::nullopt;
                }
            }
            else if (param.second == "UpdateFile")
            {
                multiRet.uploadData = std::move(formpart.content);
            }
        }
    }

    if (multiRet.uploadData.empty())
    {
        BMCWEB_LOG_ERROR("Upload data is NULL");
        messages::propertyMissing(asyncResp->res, "UpdateFile");
        return std::nullopt;
    }
    if (multiRet.targets.empty())
    {
        messages::propertyMissing(asyncResp->res, "Targets");
        return std::nullopt;
    }
    return multiRet;
}

inline void
    handleStartUpdate(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      task::Payload payload, const std::string& objectPath,
                      const boost::system::error_code& ec,
                      const sdbusplus::message::object_path& retPath)
{
    if (ec)
    {
        BMCWEB_LOG_ERROR("error_code = {}", ec);
        BMCWEB_LOG_ERROR("error msg = {}", ec.message());
        messages::internalError(asyncResp->res);
        return;
    }

    BMCWEB_LOG_INFO("Call to StartUpdate Success, retPath = {}", retPath.str);
    createTask(asyncResp, std::move(payload), objectPath);
}

inline void startUpdate(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        task::Payload payload,
                        const MemoryFileDescriptor& memfd,
                        const std::string& applyTime,
                        const std::string& objectPath,
                        const std::string& serviceName)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, payload = std::move(payload),
         objectPath](const boost::system::error_code& ec1,
                     const sdbusplus::message::object_path& retPath) mutable {
        handleStartUpdate(asyncResp, std::move(payload), objectPath, ec1,
                          retPath);
    },
        serviceName, objectPath, "xyz.openbmc_project.Software.Update",
        "StartUpdate", sdbusplus::message::unix_fd(memfd.fd), applyTime);
}

inline void getAssociatedUpdateInterface(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp, task::Payload payload,
    const MemoryFileDescriptor& memfd, const std::string& applyTime,
    const boost::system::error_code& ec,
    const dbus::utility::MapperGetSubTreeResponse& subtree)
{
    if (ec)
    {
        BMCWEB_LOG_ERROR("error_code = {}", ec);
        BMCWEB_LOG_ERROR("error msg = {}", ec.message());
        messages::internalError(asyncResp->res);
        return;
    }
    BMCWEB_LOG_DEBUG("Found {} startUpdate subtree paths", subtree.size());

    if (subtree.size() > 1)
    {
        BMCWEB_LOG_ERROR("Found more than one startUpdate subtree paths");
        messages::internalError(asyncResp->res);
        return;
    }

    auto objectPath = subtree[0].first;
    auto serviceName = subtree[0].second[0].first;

    BMCWEB_LOG_DEBUG("Found objectPath {} serviceName {}", objectPath,
                     serviceName);
    startUpdate(asyncResp, std::move(payload), memfd, applyTime, objectPath,
                serviceName);
}

inline void
    getSwInfo(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
              task::Payload payload, MemoryFileDescriptor memfd,
              const std::string& applyTime, const std::string& target,
              const boost::system::error_code& ec,
              const dbus::utility::MapperGetSubTreePathsResponse& subtree)
{
    using SwInfoMap =
        std::unordered_map<std::string, sdbusplus::message::object_path>;
    SwInfoMap swInfoMap;

    if (ec)
    {
        BMCWEB_LOG_ERROR("error_code = {}", ec);
        BMCWEB_LOG_ERROR("error msg = {}", ec.message());
        messages::internalError(asyncResp->res);
        return;
    }
    BMCWEB_LOG_DEBUG("Found {} software version paths", subtree.size());

    for (const auto& objectPath : subtree)
    {
        sdbusplus::message::object_path path(objectPath);
        std::string swId = path.filename();
        swInfoMap.emplace(swId, path);
    }

    auto swEntry = swInfoMap.find(target);
    if (swEntry == swInfoMap.end())
    {
        BMCWEB_LOG_WARNING("No valid DBus path for Target URI {}", target);
        messages::propertyValueFormatError(asyncResp->res, target, "Targets");
        return;
    }

    BMCWEB_LOG_DEBUG("Found software version path {}", swEntry->second.str);

    sdbusplus::message::object_path swObjectPath = swEntry->second /
                                                   "software_version";
    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Software.Update"};
    dbus::utility::getAssociatedSubTree(
        swObjectPath,
        sdbusplus::message::object_path("/xyz/openbmc_project/software"), 0,
        interfaces,
        [asyncResp, payload = std::move(payload), memfd = std::move(memfd),
         applyTime](
            const boost::system::error_code& ec1,
            const dbus::utility::MapperGetSubTreeResponse& subtree1) mutable {
        getAssociatedUpdateInterface(asyncResp, std::move(payload), memfd,
                                     applyTime, ec1, subtree1);
    });
}

inline void
    processUpdateRequest(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         task::Payload&& payload, std::string_view body,
                         const std::string& applyTime,
                         std::vector<std::string>& targets)
{
    MemoryFileDescriptor memfd("update-image");
    if (memfd.fd == -1)
    {
        BMCWEB_LOG_ERROR("Failed to create image memfd");
        messages::internalError(asyncResp->res);
        return;
    }
    if (write(memfd.fd, body.data(), body.length()) !=
        static_cast<ssize_t>(body.length()))
    {
        BMCWEB_LOG_ERROR("Failed to write to image memfd");
        messages::internalError(asyncResp->res);
        return;
    }
    if (!memfd.rewind())
    {
        messages::internalError(asyncResp->res);
        return;
    }

    if (!targets.empty() && targets[0] == BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        startUpdate(asyncResp, std::move(payload), memfd, applyTime,
                    "/xyz/openbmc_project/software/bmc",
                    "xyz.openbmc_project.Software.Manager");
    }
    else
    {
        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Software.Version"};
        dbus::utility::getSubTreePaths(
            "/xyz/openbmc_project/software", 1, interfaces,
            [asyncResp, payload = std::move(payload), memfd = std::move(memfd),
             applyTime,
             targets](const boost::system::error_code& ec,
                      const dbus::utility::MapperGetSubTreePathsResponse&
                          subtree) mutable {
            getSwInfo(asyncResp, std::move(payload), std::move(memfd),
                      applyTime, targets[0], ec, subtree);
        });
    }
}

inline void
    updateMultipartContext(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const crow::Request& req, MultipartParser&& parser)
{
    std::optional<MultiPartUpdateParameters> multipart =
        extractMultipartUpdateParameters(asyncResp, std::move(parser));
    if (!multipart)
    {
        return;
    }
    if (!multipart->applyTime)
    {
        multipart->applyTime = "OnReset";
    }

    if constexpr (BMCWEB_REDFISH_UPDATESERVICE_USE_DBUS)
    {
        std::string applyTimeNewVal;
        if (!convertApplyTime(asyncResp->res, *multipart->applyTime,
                              applyTimeNewVal))
        {
            return;
        }
        task::Payload payload(req);

        processUpdateRequest(asyncResp, std::move(payload),
                             multipart->uploadData, applyTimeNewVal,
                             multipart->targets);
    }
    else
    {
        setApplyTime(asyncResp, *multipart->applyTime);

        // Setup callback for when new software detected
        monitorForSoftwareAvailable(asyncResp, req,
                                    "/redfish/v1/UpdateService");

        uploadImageFile(asyncResp->res, multipart->uploadData);
    }
}

inline void doHTTPUpdate(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const crow::Request& req)
{
    if constexpr (BMCWEB_REDFISH_UPDATESERVICE_USE_DBUS)
    {
        task::Payload payload(req);
        // HTTP push only supports BMC updates (with ApplyTime as immediate) for
        // backwards compatibility. Specific component updates will be handled
        // through Multipart form HTTP push.
        std::vector<std::string> targets;
        targets.emplace_back(BMCWEB_REDFISH_MANAGER_URI_NAME);

        processUpdateRequest(
            asyncResp, std::move(payload), req.body(),
            "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate",
            targets);
    }
    else
    {
        // Setup callback for when new software detected
        monitorForSoftwareAvailable(asyncResp, req,
                                    "/redfish/v1/UpdateService");

        uploadImageFile(asyncResp->res, req.body());
    }
}

inline void
    handleUpdateServicePost(App& app, const crow::Request& req,
                            const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    std::string_view contentType = req.getHeaderValue("Content-Type");

    BMCWEB_LOG_DEBUG("doPost: contentType={}", contentType);

    // Make sure that content type is application/octet-stream or
    // multipart/form-data
    if (bmcweb::asciiIEquals(contentType, "application/octet-stream"))
    {
        doHTTPUpdate(asyncResp, req);
    }
    else if (contentType.starts_with("multipart/form-data"))
    {
        MultipartParser parser;

        ParserError ec = parser.parse(req);
        if (ec != ParserError::PARSER_SUCCESS)
        {
            // handle error
            BMCWEB_LOG_ERROR("MIME parse failed, ec : {}",
                             static_cast<int>(ec));
            messages::internalError(asyncResp->res);
            return;
        }

        updateMultipartContext(asyncResp, req, std::move(parser));
    }
    else
    {
        BMCWEB_LOG_DEBUG("Bad content type specified:{}", contentType);
        asyncResp->res.result(boost::beast::http::status::bad_request);
    }
}

inline void
    handleUpdateServiceGet(App& app, const crow::Request& req,
                           const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    asyncResp->res.jsonValue["@odata.type"] =
        "#UpdateService.v1_11_1.UpdateService";
    asyncResp->res.jsonValue["@odata.id"] = "/redfish/v1/UpdateService";
    asyncResp->res.jsonValue["Id"] = "UpdateService";
    asyncResp->res.jsonValue["Description"] = "Service for Software Update";
    asyncResp->res.jsonValue["Name"] = "Update Service";

    asyncResp->res.jsonValue["HttpPushUri"] =
        "/redfish/v1/UpdateService/update";
    asyncResp->res.jsonValue["MultipartHttpPushUri"] =
        "/redfish/v1/UpdateService/update";

    // UpdateService cannot be disabled
    asyncResp->res.jsonValue["ServiceEnabled"] = true;
    asyncResp->res.jsonValue["FirmwareInventory"]["@odata.id"] =
        "/redfish/v1/UpdateService/FirmwareInventory";
    // Get the MaxImageSizeBytes
    asyncResp->res.jsonValue["MaxImageSizeBytes"] = BMCWEB_HTTP_BODY_LIMIT *
                                                    1024 * 1024;

    // Update Actions object.
    nlohmann::json& updateSvcSimpleUpdate =
        asyncResp->res.jsonValue["Actions"]["#UpdateService.SimpleUpdate"];
    updateSvcSimpleUpdate["target"] =
        "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate";

    nlohmann::json::array_t allowed;
    allowed.emplace_back(update_service::TransferProtocolType::HTTPS);

    if constexpr (BMCWEB_INSECURE_PUSH_STYLE_NOTIFICATION)
    {
        allowed.emplace_back(update_service::TransferProtocolType::TFTP);
    }

    updateSvcSimpleUpdate["TransferProtocol@Redfish.AllowableValues"] =
        std::move(allowed);

    asyncResp->res.jsonValue["HttpPushUriOptions"]["HttpPushUriApplyTime"]
                            ["ApplyTime"] = "Immediate";
}

inline void handleUpdateServiceFirmwareInventoryCollectionGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    asyncResp->res.jsonValue["@odata.type"] =
        "#SoftwareInventoryCollection.SoftwareInventoryCollection";
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/UpdateService/FirmwareInventory";
    asyncResp->res.jsonValue["Name"] = "Software Inventory Collection";
    const std::array<const std::string_view, 1> iface = {
        "xyz.openbmc_project.Software.Version"};

    redfish::collection_util::getCollectionMembers(
        asyncResp,
        boost::urls::url("/redfish/v1/UpdateService/FirmwareInventory"), iface,
        "/xyz/openbmc_project/software");
}

/* Fill related item links (i.e. bmc, bios) in for inventory */
inline void getRelatedItems(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& purpose)
{
    if (purpose == sw_util::bmcPurpose)
    {
        nlohmann::json& relatedItem = asyncResp->res.jsonValue["RelatedItem"];
        nlohmann::json::object_t item;
        item["@odata.id"] = boost::urls::format(
            "/redfish/v1/Managers/{}", BMCWEB_REDFISH_MANAGER_URI_NAME);
        relatedItem.emplace_back(std::move(item));
        asyncResp->res.jsonValue["RelatedItem@odata.count"] =
            relatedItem.size();
    }
    else if (purpose == sw_util::biosPurpose)
    {
        nlohmann::json& relatedItem = asyncResp->res.jsonValue["RelatedItem"];
        nlohmann::json::object_t item;
        item["@odata.id"] = std::format("/redfish/v1/Systems/{}/Bios",
                                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        relatedItem.emplace_back(std::move(item));
        asyncResp->res.jsonValue["RelatedItem@odata.count"] =
            relatedItem.size();
    }
    else
    {
        BMCWEB_LOG_DEBUG("Unknown software purpose {}", purpose);
    }
}

inline void
    getSoftwareVersion(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& service, const std::string& path,
                       const std::string& swId, uint16_t hostNumber)
{
    sdbusplus::asio::getAllProperties(
        *crow::connections::systemBus, service, path,
        "xyz.openbmc_project.Software.Version",
        [asyncResp, swId,
         hostNumber](const boost::system::error_code& ec,
                     const dbus::utility::DBusPropertiesMap& propertiesList) {
        if (ec)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        const std::string* swInvPurpose = nullptr;
        const std::string* version = nullptr;

        const bool success = sdbusplus::unpackPropertiesNoThrow(
            dbus_utils::UnpackErrorPrinter(), propertiesList, "Purpose",
            swInvPurpose, "Version", version);

        if (swId == "bios_active")
        {
            const std::vector<std::string>* hostVersions = nullptr;
            sdbusplus::unpackPropertiesNoThrow(dbus_utils::UnpackErrorPrinter(),
                                               propertiesList, "HostVersions",
                                               hostVersions);

            if (hostVersions != nullptr && hostNumber < hostVersions->size())
            {
                version = &(*hostVersions)[hostNumber];
            }
        }

        if (!success)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        if (swInvPurpose == nullptr)
        {
            BMCWEB_LOG_DEBUG("Can't find property \"Purpose\"!");
            messages::internalError(asyncResp->res);
            return;
        }

        BMCWEB_LOG_DEBUG("swInvPurpose = {}", *swInvPurpose);

        if (version == nullptr)
        {
            BMCWEB_LOG_DEBUG("Can't find property \"Version\"!");

            messages::internalError(asyncResp->res);

            return;
        }
        asyncResp->res.jsonValue["Version"] = *version;
        asyncResp->res.jsonValue["Id"] = swId;

        // swInvPurpose is of format:
        // xyz.openbmc_project.Software.Version.VersionPurpose.ABC
        // Translate this to "ABC image"
        size_t endDesc = swInvPurpose->rfind('.');
        if (endDesc == std::string::npos)
        {
            messages::internalError(asyncResp->res);
            return;
        }
        endDesc++;
        if (endDesc >= swInvPurpose->size())
        {
            messages::internalError(asyncResp->res);
            return;
        }

        std::string formatDesc = swInvPurpose->substr(endDesc);
        asyncResp->res.jsonValue["Description"] = formatDesc + " image";
        getRelatedItems(asyncResp, *swInvPurpose);
    });
}

inline void handleUpdateServiceFirmwareInventoryGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& param)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    boost::urls::url_view urlView = req.url();
    uint16_t hostNumber;

    for (const auto& parameter : urlView.params())
    {
        if (parameter.key == "HostNumber" && !parameter.value.empty())
        {
            try
            {
                int temp = std::stoi(std::string(parameter.value));
                hostNumber = static_cast<uint16_t>(temp);
            }
            catch (const std::exception& e)
            {
                BMCWEB_LOG_WARNING("Invalid HostNumber format: {}",
                                   parameter.value);
                hostNumber = 0;
            }
            break;
        }
    }

    if (hostNumber > 2)
    {
        messages::actionParameterNotSupported(
            asyncResp->res, std::to_string(hostNumber), "HostNumber");
    }

    std::shared_ptr<std::string> swId = std::make_shared<std::string>(param);

    asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
        "/redfish/v1/UpdateService/FirmwareInventory/{}", *swId);

    constexpr std::array<std::string_view, 1> interfaces = {
        "xyz.openbmc_project.Software.Version"};
    dbus::utility::getSubTree(
        "/", 0, interfaces,
        [asyncResp, swId,
         hostNumber](const boost::system::error_code& ec,
                     const dbus::utility::MapperGetSubTreeResponse& subtree) {
        BMCWEB_LOG_DEBUG("doGet callback...");
        if (ec)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        // Ensure we find our input swId, otherwise return an error
        bool found = false;
        for (const std::pair<
                 std::string,
                 std::vector<std::pair<std::string, std::vector<std::string>>>>&
                 obj : subtree)
        {
            if (!obj.first.ends_with(*swId))
            {
                continue;
            }

            if (obj.second.empty())
            {
                continue;
            }

            found = true;
            sw_util::getSwStatus(asyncResp, swId, obj.second[0].first);

            if (*swId == "vr_bundle_active")
            {
                sw_util::getVRBundleFw(asyncResp, swId, obj.second[0].first);
            }

            getSoftwareVersion(asyncResp, obj.second[0].first, obj.first, *swId,
                               hostNumber);
        }
        if (!found)
        {
            BMCWEB_LOG_WARNING("Input swID {} not found!", *swId);
            messages::resourceMissingAtURI(
                asyncResp->res,
                boost::urls::format(
                    "/redfish/v1/UpdateService/FirmwareInventory/{}", *swId));
            return;
        }
        asyncResp->res.jsonValue["@odata.type"] =
            "#SoftwareInventory.v1_1_0.SoftwareInventory";
        asyncResp->res.jsonValue["Name"] = "Software Inventory";
        asyncResp->res.jsonValue["Status"]["HealthRollup"] = "OK";

        asyncResp->res.jsonValue["Updateable"] = false;
        sw_util::getSwUpdatableStatus(asyncResp, swId);
    });
}

inline void requestRoutesUpdateService(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/UpdateService/Actions/UpdateService.SimpleUpdate/")
        .privileges(redfish::privileges::postUpdateService)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleUpdateServiceSimpleUpdateAction, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/FirmwareInventory/<str>/")
        .privileges(redfish::privileges::getSoftwareInventory)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleUpdateServiceFirmwareInventoryGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/")
        .privileges(redfish::privileges::getUpdateService)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleUpdateServiceGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/update/")
        .privileges(redfish::privileges::postUpdateService)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleUpdateServicePost, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/UpdateService/FirmwareInventory/")
        .privileges(redfish::privileges::getSoftwareInventoryCollection)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleUpdateServiceFirmwareInventoryCollectionGet, std::ref(app)));
}

} // namespace redfish
