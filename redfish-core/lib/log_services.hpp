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

#include "app.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "generated/enums/log_entry.hpp"
#include "gzfile.hpp"
#include "http_utility.hpp"
#include "human_sort.hpp"
#include "query.hpp"
#include "registries.hpp"
#include "registries/base_message_registry.hpp"
#include "registries/openbmc_message_registry.hpp"
#include "registries/privilege_registry.hpp"
#include "task.hpp"
#include "task_messages.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/time_utils.hpp"

#include <systemd/sd-id128.h>
#include <systemd/sd-journal.h>
#include <tinyxml2.h>
#include <unistd.h>

#include <boost/beast/http/verb.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/system/linux_error.hpp>
#include <boost/url/format.hpp>
#include <boost/url/params_view.hpp>
#include <boost/url/url_view.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/unpack_properties.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace redfish
{

constexpr const char* crashdumpObject = "com.amd.RAS";
constexpr const char* crashdumpPath = "/com/amd/RAS";
constexpr const char* crashdumpInterface = "com.amd.crashdump";
constexpr const char* deleteAllInterface =
    "xyz.openbmc_project.Collection.DeleteAll";
constexpr const char* crashdumpOnDemandInterface =
    "com.intel.crashdump.OnDemand";
constexpr const char* crashdumpTelemetryInterface =
    "com.intel.crashdump.Telemetry";

constexpr const char* pprFileObject = "xyz.openbmc_project.PostPackageRepair";
constexpr const char* pprFilePath = "/xyz/openbmc_project/PostPackageRepair";
constexpr const char* pprFileInterface =
    "xyz.openbmc_project.PostPackageRepair.PprData";

enum class DumpCreationProgress
{
    DUMP_CREATE_SUCCESS,
    DUMP_CREATE_FAILED,
    DUMP_CREATE_INPROGRESS
};

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

namespace fs = std::filesystem;

inline std::string translateSeverityDbusToRedfish(const std::string& s)
{
    if ((s == "xyz.openbmc_project.Logging.Entry.Level.Alert") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Critical") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Emergency") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Error"))
    {
        return "Critical";
    }
    if ((s == "xyz.openbmc_project.Logging.Entry.Level.Debug") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Informational") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Notice"))
    {
        return "OK";
    }
    if (s == "xyz.openbmc_project.Logging.Entry.Level.Warning")
    {
        return "Warning";
    }
    return "";
}

inline std::optional<bool> getProviderNotifyAction(const std::string& notify)
{
    std::optional<bool> notifyAction;
    if (notify == "xyz.openbmc_project.Logging.Entry.Notify.Notify")
    {
        notifyAction = true;
    }
    else if (notify == "xyz.openbmc_project.Logging.Entry.Notify.Inhibit")
    {
        notifyAction = false;
    }

    return notifyAction;
}

inline std::string getDumpPath(std::string_view dumpType)
{
    std::string dbusDumpPath = "/xyz/openbmc_project/dump/";
    std::ranges::transform(dumpType, std::back_inserter(dbusDumpPath),
                           bmcweb::asciiToLower);

    return dbusDumpPath;
}

inline int getJournalMetadata(sd_journal* journal, std::string_view field,
                              std::string_view& contents)
{
    const char* data = nullptr;
    size_t length = 0;
    int ret = 0;
    // Get the metadata from the requested field of the journal entry
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const void** dataVoid = reinterpret_cast<const void**>(&data);

    ret = sd_journal_get_data(journal, field.data(), dataVoid, &length);
    if (ret < 0)
    {
        return ret;
    }
    contents = std::string_view(data, length);
    // Only use the content after the "=" character.
    contents.remove_prefix(std::min(contents.find('=') + 1, contents.size()));
    return ret;
}

inline int getJournalMetadata(sd_journal* journal, std::string_view field,
                              const int& base, long int& contents)
{
    int ret = 0;
    std::string_view metadata;
    // Get the metadata from the requested field of the journal entry
    ret = getJournalMetadata(journal, field, metadata);
    if (ret < 0)
    {
        return ret;
    }
    contents = strtol(metadata.data(), nullptr, base);
    return ret;
}

inline bool getEntryTimestamp(sd_journal* journal, std::string& entryTimestamp)
{
    int ret = 0;
    uint64_t timestamp = 0;
    ret = sd_journal_get_realtime_usec(journal, &timestamp);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR("Failed to read entry timestamp: {}", strerror(-ret));
        return false;
    }
    entryTimestamp = redfish::time_utils::getDateTimeUintUs(timestamp);
    return true;
}

inline bool getUniqueEntryID(sd_journal* journal, std::string& entryID,
                             const bool firstEntry = true)
{
    int ret = 0;
    static sd_id128_t prevBootID{};
    static uint64_t prevTs = 0;
    static int index = 0;
    if (firstEntry)
    {
        prevBootID = {};
        prevTs = 0;
    }

    // Get the entry timestamp
    uint64_t curTs = 0;
    sd_id128_t curBootID{};
    ret = sd_journal_get_monotonic_usec(journal, &curTs, &curBootID);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR("Failed to read entry timestamp: {}", strerror(-ret));
        return false;
    }
    // If the timestamp isn't unique on the same boot, increment the index
    bool sameBootIDs = sd_id128_equal(curBootID, prevBootID) != 0;
    if (sameBootIDs && (curTs == prevTs))
    {
        index++;
    }
    else
    {
        // Otherwise, reset it
        index = 0;
    }

    if (!sameBootIDs)
    {
        // Save the bootID
        prevBootID = curBootID;
    }
    // Save the timestamp
    prevTs = curTs;

    // make entryID as <bootID>_<timestamp>[_<index>]
    std::array<char, SD_ID128_STRING_MAX> bootIDStr{};
    sd_id128_to_string(curBootID, bootIDStr.data());
    entryID = std::format("{}_{}", bootIDStr.data(), curTs);
    if (index > 0)
    {
        entryID += "_" + std::to_string(index);
    }
    return true;
}

static bool getUniqueEntryID(const std::string& logEntry, std::string& entryID,
                             const bool firstEntry = true)
{
    static time_t prevTs = 0;
    static int index = 0;
    if (firstEntry)
    {
        prevTs = 0;
    }

    // Get the entry timestamp
    std::time_t curTs = 0;
    std::tm timeStruct = {};
    std::istringstream entryStream(logEntry);
    if (entryStream >> std::get_time(&timeStruct, "%Y-%m-%dT%H:%M:%S"))
    {
        curTs = std::mktime(&timeStruct);
    }
    // If the timestamp isn't unique, increment the index
    if (curTs == prevTs)
    {
        index++;
    }
    else
    {
        // Otherwise, reset it
        index = 0;
    }
    // Save the timestamp
    prevTs = curTs;

    entryID = std::to_string(curTs);
    if (index > 0)
    {
        entryID += "_" + std::to_string(index);
    }
    return true;
}

// Entry is formed like "BootID_timestamp" or "BootID_timestamp_index"
inline bool
    getTimestampFromID(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       std::string_view entryIDStrView, sd_id128_t& bootID,
                       uint64_t& timestamp, uint64_t& index)
{
    // Convert the unique ID back to a bootID + timestamp to find the entry
    auto underscore1Pos = entryIDStrView.find('_');
    if (underscore1Pos == std::string_view::npos)
    {
        // EntryID has no bootID or timestamp
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryIDStrView);
        return false;
    }

    // EntryID has bootID + timestamp

    // Convert entryIDViewString to BootID
    // NOTE: bootID string which needs to be null-terminated for
    // sd_id128_from_string()
    std::string bootIDStr(entryIDStrView.substr(0, underscore1Pos));
    if (sd_id128_from_string(bootIDStr.c_str(), &bootID) < 0)
    {
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryIDStrView);
        return false;
    }

    // Get the timestamp from entryID
    entryIDStrView.remove_prefix(underscore1Pos + 1);

    auto [timestampEnd, tstampEc] = std::from_chars(
        entryIDStrView.begin(), entryIDStrView.end(), timestamp);
    if (tstampEc != std::errc())
    {
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryIDStrView);
        return false;
    }
    entryIDStrView = std::string_view(
        timestampEnd,
        static_cast<size_t>(std::distance(timestampEnd, entryIDStrView.end())));
    if (entryIDStrView.empty())
    {
        index = 0U;
        return true;
    }
    // Timestamp might include optional index, if two events happened at the
    // same "time".
    if (entryIDStrView[0] != '_')
    {
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryIDStrView);
        return false;
    }
    entryIDStrView.remove_prefix(1);
    auto [ptr, indexEc] = std::from_chars(entryIDStrView.begin(),
                                          entryIDStrView.end(), index);
    if (indexEc != std::errc() || ptr != entryIDStrView.end())
    {
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryIDStrView);
        return false;
    }
    return true;
}

static bool
    getRedfishLogFiles(std::vector<std::filesystem::path>& redfishLogFiles)
{
    static const std::filesystem::path redfishLogDir = "/var/log";
    static const std::string redfishLogFilename = "redfish";

    // Loop through the directory looking for redfish log files
    for (const std::filesystem::directory_entry& dirEnt :
         std::filesystem::directory_iterator(redfishLogDir))
    {
        // If we find a redfish log file, save the path
        std::string filename = dirEnt.path().filename();
        if (filename.starts_with(redfishLogFilename))
        {
            redfishLogFiles.emplace_back(redfishLogDir / filename);
        }
    }
    // As the log files rotate, they are appended with a ".#" that is higher for
    // the older logs. Since we don't expect more than 10 log files, we
    // can just sort the list to get them in order from newest to oldest
    std::ranges::sort(redfishLogFiles);

    return !redfishLogFiles.empty();
}

inline log_entry::OriginatorTypes
    mapDbusOriginatorTypeToRedfish(const std::string& originatorType)
{
    if (originatorType ==
        "xyz.openbmc_project.Common.OriginatedBy.OriginatorTypes.Client")
    {
        return log_entry::OriginatorTypes::Client;
    }
    if (originatorType ==
        "xyz.openbmc_project.Common.OriginatedBy.OriginatorTypes.Internal")
    {
        return log_entry::OriginatorTypes::Internal;
    }
    if (originatorType ==
        "xyz.openbmc_project.Common.OriginatedBy.OriginatorTypes.SupportingService")
    {
        return log_entry::OriginatorTypes::SupportingService;
    }
    return log_entry::OriginatorTypes::Invalid;
}

inline void parseDumpEntryFromDbusObject(
    const dbus::utility::ManagedObjectType::value_type& object,
    std::string& dumpStatus, uint64_t& size, uint64_t& timestampUs,
    std::string& originatorId, log_entry::OriginatorTypes& originatorType,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    for (const auto& interfaceMap : object.second)
    {
        if (interfaceMap.first == "xyz.openbmc_project.Common.Progress")
        {
            for (const auto& propertyMap : interfaceMap.second)
            {
                if (propertyMap.first == "Status")
                {
                    const auto* status =
                        std::get_if<std::string>(&propertyMap.second);
                    if (status == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    dumpStatus = *status;
                }
            }
        }
        else if (interfaceMap.first == "xyz.openbmc_project.Dump.Entry")
        {
            for (const auto& propertyMap : interfaceMap.second)
            {
                if (propertyMap.first == "Size")
                {
                    const auto* sizePtr =
                        std::get_if<uint64_t>(&propertyMap.second);
                    if (sizePtr == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    size = *sizePtr;
                    break;
                }
            }
        }
        else if (interfaceMap.first == "xyz.openbmc_project.Time.EpochTime")
        {
            for (const auto& propertyMap : interfaceMap.second)
            {
                if (propertyMap.first == "Elapsed")
                {
                    const uint64_t* usecsTimeStamp =
                        std::get_if<uint64_t>(&propertyMap.second);
                    if (usecsTimeStamp == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    timestampUs = *usecsTimeStamp;
                    break;
                }
            }
        }
        else if (interfaceMap.first ==
                 "xyz.openbmc_project.Common.OriginatedBy")
        {
            for (const auto& propertyMap : interfaceMap.second)
            {
                if (propertyMap.first == "OriginatorId")
                {
                    const std::string* id =
                        std::get_if<std::string>(&propertyMap.second);
                    if (id == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    originatorId = *id;
                }

                if (propertyMap.first == "OriginatorType")
                {
                    const std::string* type =
                        std::get_if<std::string>(&propertyMap.second);
                    if (type == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }

                    originatorType = mapDbusOriginatorTypeToRedfish(*type);
                    if (originatorType == log_entry::OriginatorTypes::Invalid)
                    {
                        messages::internalError(asyncResp->res);
                        break;
                    }
                }
            }
        }
    }
}

static std::string getDumpEntriesPath(const std::string& dumpType)
{
    std::string entriesPath;

    if (dumpType == "BMC")
    {
        entriesPath =
            std::format("/redfish/v1/Managers/{}/LogServices/Dump/Entries/",
                        BMCWEB_REDFISH_MANAGER_URI_NAME);
    }
    else if (dumpType == "FaultLog")
    {
        entriesPath =
            std::format("/redfish/v1/Managers/{}/LogServices/FaultLog/Entries/",
                        BMCWEB_REDFISH_MANAGER_URI_NAME);
    }
    else if (dumpType == "System")
    {
        entriesPath =
            std::format("/redfish/v1/Systems/{}/LogServices/Dump/Entries/",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
    }
    else
    {
        BMCWEB_LOG_ERROR("getDumpEntriesPath() invalid dump type: {}",
                         dumpType);
    }

    // Returns empty string on error
    return entriesPath;
}

inline void
    getDumpEntryCollection(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& dumpType)
{
    std::string entriesPath = getDumpEntriesPath(dumpType);
    if (entriesPath.empty())
    {
        messages::internalError(asyncResp->res);
        return;
    }

    sdbusplus::message::object_path path("/xyz/openbmc_project/dump");
    dbus::utility::getManagedObjects(
        "xyz.openbmc_project.Dump.Manager", path,
        [asyncResp, entriesPath,
         dumpType](const boost::system::error_code& ec,
                   const dbus::utility::ManagedObjectType& objects) {
        if (ec)
        {
            BMCWEB_LOG_ERROR("DumpEntry resp_handler got error {}", ec);
            messages::internalError(asyncResp->res);
            return;
        }

        // Remove ending slash
        std::string odataIdStr = entriesPath;
        if (!odataIdStr.empty())
        {
            odataIdStr.pop_back();
        }

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] = std::move(odataIdStr);
        asyncResp->res.jsonValue["Name"] = dumpType + " Dump Entries";
        asyncResp->res.jsonValue["Description"] = "Collection of " + dumpType +
                                                  " Dump Entries";

        nlohmann::json::array_t entriesArray;
        std::string dumpEntryPath = getDumpPath(dumpType) + "/entry/";

        dbus::utility::ManagedObjectType resp(objects);
        std::ranges::sort(resp, [](const auto& l, const auto& r) {
            return AlphanumLess<std::string>()(l.first.filename(),
                                               r.first.filename());
        });

        for (auto& object : resp)
        {
            if (object.first.str.find(dumpEntryPath) == std::string::npos)
            {
                continue;
            }
            uint64_t timestampUs = 0;
            uint64_t size = 0;
            std::string dumpStatus;
            std::string originatorId;
            log_entry::OriginatorTypes originatorType =
                log_entry::OriginatorTypes::Internal;
            nlohmann::json::object_t thisEntry;

            std::string entryID = object.first.filename();
            if (entryID.empty())
            {
                continue;
            }

            parseDumpEntryFromDbusObject(object, dumpStatus, size, timestampUs,
                                         originatorId, originatorType,
                                         asyncResp);

            if (dumpStatus !=
                    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed" &&
                !dumpStatus.empty())
            {
                // Dump status is not Complete, no need to enumerate
                continue;
            }

            thisEntry["@odata.type"] = "#LogEntry.v1_11_0.LogEntry";
            thisEntry["@odata.id"] = entriesPath + entryID;
            thisEntry["Id"] = entryID;
            thisEntry["EntryType"] = "Event";
            thisEntry["Name"] = dumpType + " Dump Entry";
            thisEntry["Created"] =
                redfish::time_utils::getDateTimeUintUs(timestampUs);

            if (!originatorId.empty())
            {
                thisEntry["Originator"] = originatorId;
                thisEntry["OriginatorType"] = originatorType;
            }

            if (dumpType == "BMC")
            {
                thisEntry["DiagnosticDataType"] = "Manager";
                thisEntry["AdditionalDataURI"] = entriesPath + entryID +
                                                 "/attachment";
                thisEntry["AdditionalDataSizeBytes"] = size;
            }
            else if (dumpType == "System")
            {
                thisEntry["DiagnosticDataType"] = "OEM";
                thisEntry["OEMDiagnosticDataType"] = "System";
                thisEntry["AdditionalDataURI"] = entriesPath + entryID +
                                                 "/attachment";
                thisEntry["AdditionalDataSizeBytes"] = size;
            }
            entriesArray.emplace_back(std::move(thisEntry));
        }
        asyncResp->res.jsonValue["Members@odata.count"] = entriesArray.size();
        asyncResp->res.jsonValue["Members"] = std::move(entriesArray);
    });
}

inline void
    getDumpEntryById(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                     const std::string& entryID, const std::string& dumpType)
{
    std::string entriesPath = getDumpEntriesPath(dumpType);
    if (entriesPath.empty())
    {
        messages::internalError(asyncResp->res);
        return;
    }

    sdbusplus::message::object_path path("/xyz/openbmc_project/dump");
    dbus::utility::getManagedObjects(
        "xyz.openbmc_project.Dump.Manager", path,
        [asyncResp, entryID, dumpType,
         entriesPath](const boost::system::error_code& ec,
                      const dbus::utility::ManagedObjectType& resp) {
        if (ec)
        {
            BMCWEB_LOG_ERROR("DumpEntry resp_handler got error {}", ec);
            messages::internalError(asyncResp->res);
            return;
        }

        bool foundDumpEntry = false;
        std::string dumpEntryPath = getDumpPath(dumpType) + "/entry/";

        for (const auto& objectPath : resp)
        {
            if (objectPath.first.str != dumpEntryPath + entryID)
            {
                continue;
            }

            foundDumpEntry = true;
            uint64_t timestampUs = 0;
            uint64_t size = 0;
            std::string dumpStatus;
            std::string originatorId;
            log_entry::OriginatorTypes originatorType =
                log_entry::OriginatorTypes::Internal;

            parseDumpEntryFromDbusObject(objectPath, dumpStatus, size,
                                         timestampUs, originatorId,
                                         originatorType, asyncResp);

            if (dumpStatus !=
                    "xyz.openbmc_project.Common.Progress.OperationStatus.Completed" &&
                !dumpStatus.empty())
            {
                // Dump status is not Complete
                // return not found until status is changed to Completed
                messages::resourceNotFound(asyncResp->res, dumpType + " dump",
                                           entryID);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntry.v1_11_0.LogEntry";
            asyncResp->res.jsonValue["@odata.id"] = entriesPath + entryID;
            asyncResp->res.jsonValue["Id"] = entryID;
            asyncResp->res.jsonValue["EntryType"] = "Event";
            asyncResp->res.jsonValue["Name"] = dumpType + " Dump Entry";
            asyncResp->res.jsonValue["Created"] =
                redfish::time_utils::getDateTimeUintUs(timestampUs);

            if (!originatorId.empty())
            {
                asyncResp->res.jsonValue["Originator"] = originatorId;
                asyncResp->res.jsonValue["OriginatorType"] = originatorType;
            }

            if (dumpType == "BMC")
            {
                asyncResp->res.jsonValue["DiagnosticDataType"] = "Manager";
                asyncResp->res.jsonValue["AdditionalDataURI"] =
                    entriesPath + entryID + "/attachment";
                asyncResp->res.jsonValue["AdditionalDataSizeBytes"] = size;
            }
            else if (dumpType == "System")
            {
                asyncResp->res.jsonValue["DiagnosticDataType"] = "OEM";
                asyncResp->res.jsonValue["OEMDiagnosticDataType"] = "System";
                asyncResp->res.jsonValue["AdditionalDataURI"] =
                    entriesPath + entryID + "/attachment";
                asyncResp->res.jsonValue["AdditionalDataSizeBytes"] = size;
            }
        }
        if (!foundDumpEntry)
        {
            BMCWEB_LOG_WARNING("Can't find Dump Entry {}", entryID);
            messages::resourceNotFound(asyncResp->res, dumpType + " dump",
                                       entryID);
            return;
        }
    });
}

inline void deleteDumpEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& entryID,
                            const std::string& dumpType)
{
    auto respHandler = [asyncResp,
                        entryID](const boost::system::error_code& ec) {
        BMCWEB_LOG_DEBUG("Dump Entry doDelete callback: Done");
        if (ec)
        {
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", entryID);
                return;
            }
            BMCWEB_LOG_ERROR(
                "Dump (DBus) doDelete respHandler got error {} entryID={}", ec,
                entryID);
            messages::internalError(asyncResp->res);
            return;
        }
    };

    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Dump.Manager",
        std::format("{}/entry/{}", getDumpPath(dumpType), entryID),
        "xyz.openbmc_project.Object.Delete", "Delete");
}
inline bool checkSizeLimit(int fd, crow::Response& res)
{
    long long int size = lseek(fd, 0, SEEK_END);
    if (size <= 0)
    {
        BMCWEB_LOG_ERROR("Failed to get size of file, lseek() returned {}",
                         size);
        messages::internalError(res);
        return false;
    }

    // Arbitrary max size of 20MB to accommodate BMC dumps
    constexpr long long int maxFileSize = 20LL * 1024LL * 1024LL;
    if (size > maxFileSize)
    {
        BMCWEB_LOG_ERROR("File size {} exceeds maximum allowed size of {}",
                         size, maxFileSize);
        messages::internalError(res);
        return false;
    }
    off_t rc = lseek(fd, 0, SEEK_SET);
    if (rc < 0)
    {
        BMCWEB_LOG_ERROR("Failed to reset file offset to 0");
        messages::internalError(res);
        return false;
    }
    return true;
}
inline void
    downloadEntryCallback(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& entryID,
                          const std::string& downloadEntryType,
                          const boost::system::error_code& ec,
                          const sdbusplus::message::unix_fd& unixfd)
{
    if (ec.value() == EBADR)
    {
        messages::resourceNotFound(asyncResp->res, "EntryAttachment", entryID);
        return;
    }
    if (ec)
    {
        BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
        messages::internalError(asyncResp->res);
        return;
    }

    // Make sure we know how to process the retrieved entry attachment
    if ((downloadEntryType != "BMC") && (downloadEntryType != "System"))
    {
        BMCWEB_LOG_ERROR("downloadEntryCallback() invalid entry type: {}",
                         downloadEntryType);
        messages::internalError(asyncResp->res);
    }

    int fd = -1;
    fd = dup(unixfd);
    if (fd < 0)
    {
        BMCWEB_LOG_ERROR("Failed to open file");
        messages::internalError(asyncResp->res);
        return;
    }
    if (!checkSizeLimit(fd, asyncResp->res))
    {
        close(fd);
        return;
    }
    if (downloadEntryType == "System")
    {
        if (!asyncResp->res.openFd(fd, bmcweb::EncodingType::Base64))
        {
            messages::internalError(asyncResp->res);
            close(fd);
            return;
        }
        asyncResp->res.addHeader(
            boost::beast::http::field::content_transfer_encoding, "Base64");
        return;
    }
    if (!asyncResp->res.openFd(fd))
    {
        messages::internalError(asyncResp->res);
        close(fd);
        return;
    }
    asyncResp->res.addHeader(boost::beast::http::field::content_type,
                             "application/octet-stream");
}

inline void
    downloadDumpEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& entryID, const std::string& dumpType)
{
    if (dumpType != "BMC")
    {
        BMCWEB_LOG_WARNING("Can't find Dump Entry {}", entryID);
        messages::resourceNotFound(asyncResp->res, dumpType + " dump", entryID);
        return;
    }

    std::string dumpEntryPath = std::format("{}/entry/{}",
                                            getDumpPath(dumpType), entryID);

    auto downloadDumpEntryHandler =
        [asyncResp, entryID,
         dumpType](const boost::system::error_code& ec,
                   const sdbusplus::message::unix_fd& unixfd) {
        downloadEntryCallback(asyncResp, entryID, dumpType, ec, unixfd);
    };

    crow::connections::systemBus->async_method_call(
        std::move(downloadDumpEntryHandler), "xyz.openbmc_project.Dump.Manager",
        dumpEntryPath, "xyz.openbmc_project.Dump.Entry", "GetFileHandle");
}

inline void
    downloadEventLogEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          const std::string& systemName,
                          const std::string& entryID,
                          const std::string& dumpType)
{
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

    std::string entryPath =
        sdbusplus::message::object_path("/xyz/openbmc_project/logging/entry") /
        entryID;

    auto downloadEventLogEntryHandler =
        [asyncResp, entryID,
         dumpType](const boost::system::error_code& ec,
                   const sdbusplus::message::unix_fd& unixfd) {
        downloadEntryCallback(asyncResp, entryID, dumpType, ec, unixfd);
    };

    crow::connections::systemBus->async_method_call(
        std::move(downloadEventLogEntryHandler), "xyz.openbmc_project.Logging",
        entryPath, "xyz.openbmc_project.Logging.Entry", "GetEntry");
}

inline DumpCreationProgress
    mapDbusStatusToDumpProgress(const std::string& status)
{
    if (status ==
            "xyz.openbmc_project.Common.Progress.OperationStatus.Failed" ||
        status == "xyz.openbmc_project.Common.Progress.OperationStatus.Aborted")
    {
        return DumpCreationProgress::DUMP_CREATE_FAILED;
    }
    if (status ==
        "xyz.openbmc_project.Common.Progress.OperationStatus.Completed")
    {
        return DumpCreationProgress::DUMP_CREATE_SUCCESS;
    }
    return DumpCreationProgress::DUMP_CREATE_INPROGRESS;
}

inline DumpCreationProgress
    getDumpCompletionStatus(const dbus::utility::DBusPropertiesMap& values)
{
    for (const auto& [key, val] : values)
    {
        if (key == "Status")
        {
            const std::string* value = std::get_if<std::string>(&val);
            if (value == nullptr)
            {
                BMCWEB_LOG_ERROR("Status property value is null");
                return DumpCreationProgress::DUMP_CREATE_FAILED;
            }
            return mapDbusStatusToDumpProgress(*value);
        }
    }
    return DumpCreationProgress::DUMP_CREATE_INPROGRESS;
}

inline std::string getDumpEntryPath(const std::string& dumpPath)
{
    if (dumpPath == "/xyz/openbmc_project/dump/bmc/entry")
    {
        return std::format("/redfish/v1/Managers/{}/LogServices/Dump/Entries/",
                           BMCWEB_REDFISH_SYSTEM_URI_NAME);
    }
    if (dumpPath == "/xyz/openbmc_project/dump/system/entry")
    {
        return std::format("/redfish/v1/Systems/{}/LogServices/Dump/Entries/",
                           BMCWEB_REDFISH_SYSTEM_URI_NAME);
    }
    return "";
}

inline void createDumpTaskCallback(
    task::Payload&& payload,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& createdObjPath)
{
    const std::string dumpPath = createdObjPath.parent_path().str;
    const std::string dumpId = createdObjPath.filename();

    std::string dumpEntryPath = getDumpEntryPath(dumpPath);

    if (dumpEntryPath.empty())
    {
        BMCWEB_LOG_ERROR("Invalid dump type received");
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp, payload = std::move(payload), createdObjPath,
         dumpEntryPath{std::move(dumpEntryPath)},
         dumpId](const boost::system::error_code& ec,
                 const std::string& introspectXml) {
        if (ec)
        {
            BMCWEB_LOG_ERROR("Introspect call failed with error: {}",
                             ec.message());
            messages::internalError(asyncResp->res);
            return;
        }

        // Check if the created dump object has implemented Progress
        // interface to track dump completion. If yes, fetch the "Status"
        // property of the interface, modify the task state accordingly.
        // Else, return task completed.
        tinyxml2::XMLDocument doc;

        doc.Parse(introspectXml.data(), introspectXml.size());
        tinyxml2::XMLNode* pRoot = doc.FirstChildElement("node");
        if (pRoot == nullptr)
        {
            BMCWEB_LOG_ERROR("XML document failed to parse");
            messages::internalError(asyncResp->res);
            return;
        }
        tinyxml2::XMLElement* interfaceNode =
            pRoot->FirstChildElement("interface");

        bool isProgressIntfPresent = false;
        while (interfaceNode != nullptr)
        {
            const char* thisInterfaceName = interfaceNode->Attribute("name");
            if (thisInterfaceName != nullptr)
            {
                if (thisInterfaceName ==
                    std::string_view("xyz.openbmc_project.Common.Progress"))
                {
                    interfaceNode =
                        interfaceNode->NextSiblingElement("interface");
                    continue;
                }
                isProgressIntfPresent = true;
                break;
            }
            interfaceNode = interfaceNode->NextSiblingElement("interface");
        }

        std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
            [createdObjPath, dumpEntryPath, dumpId, isProgressIntfPresent](
                const boost::system::error_code& ec2, sdbusplus::message_t& msg,
                const std::shared_ptr<task::TaskData>& taskData) {
            if (ec2)
            {
                BMCWEB_LOG_ERROR("{}: Error in creating dump",
                                 createdObjPath.str);
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Cancelled";
                return task::completed;
            }

            if (isProgressIntfPresent)
            {
                dbus::utility::DBusPropertiesMap values;
                std::string prop;
                msg.read(prop, values);

                DumpCreationProgress dumpStatus =
                    getDumpCompletionStatus(values);
                if (dumpStatus == DumpCreationProgress::DUMP_CREATE_FAILED)
                {
                    BMCWEB_LOG_ERROR("{}: Error in creating dump",
                                     createdObjPath.str);
                    taskData->state = "Cancelled";
                    return task::completed;
                }

                if (dumpStatus == DumpCreationProgress::DUMP_CREATE_INPROGRESS)
                {
                    BMCWEB_LOG_DEBUG("{}: Dump creation task is in progress",
                                     createdObjPath.str);
                    return !task::completed;
                }
            }

            nlohmann::json retMessage = messages::success();
            taskData->messages.emplace_back(retMessage);

            boost::urls::url url = boost::urls::format(
                "/redfish/v1/Managers/{}/LogServices/Dump/Entries/{}",
                BMCWEB_REDFISH_MANAGER_URI_NAME, dumpId);

            std::string headerLoc = "Location: ";
            headerLoc += url.buffer();

            taskData->payload->httpHeaders.emplace_back(std::move(headerLoc));

            BMCWEB_LOG_DEBUG("{}: Dump creation task completed",
                             createdObjPath.str);
            taskData->state = "Completed";
            return task::completed;
        },
            "type='signal',interface='org.freedesktop.DBus.Properties',"
            "member='PropertiesChanged',path='" +
                createdObjPath.str + "'");

        // The task timer is set to max time limit within which the
        // requested dump will be collected.
        task->startTimer(std::chrono::minutes(6));
        task->populateResp(asyncResp->res);
        task->payload.emplace(payload);
    },
        "xyz.openbmc_project.Dump.Manager", createdObjPath,
        "org.freedesktop.DBus.Introspectable", "Introspect");
}

inline void createDump(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const crow::Request& req, const std::string& dumpType)
{
    std::string dumpPath = getDumpEntriesPath(dumpType);
    if (dumpPath.empty())
    {
        messages::internalError(asyncResp->res);
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

    if (dumpType == "System")
    {
        if (!oemDiagnosticDataType || !diagnosticDataType)
        {
            BMCWEB_LOG_ERROR(
                "CreateDump action parameter 'DiagnosticDataType'/'OEMDiagnosticDataType' value not found!");
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData",
                "DiagnosticDataType & OEMDiagnosticDataType");
            return;
        }
        if ((*oemDiagnosticDataType != "System") ||
            (*diagnosticDataType != "OEM"))
        {
            BMCWEB_LOG_ERROR("Wrong parameter values passed");
            messages::internalError(asyncResp->res);
            return;
        }
        dumpPath = std::format("/redfish/v1/Systems/{}/LogServices/Dump/",
                               BMCWEB_REDFISH_SYSTEM_URI_NAME);
    }
    else if (dumpType == "BMC")
    {
        if (!diagnosticDataType)
        {
            BMCWEB_LOG_ERROR(
                "CreateDump action parameter 'DiagnosticDataType' not found!");
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData", "DiagnosticDataType");
            return;
        }
        if (*diagnosticDataType != "Manager")
        {
            BMCWEB_LOG_ERROR(
                "Wrong parameter value passed for 'DiagnosticDataType'");
            messages::internalError(asyncResp->res);
            return;
        }
        dumpPath = std::format("/redfish/v1/Managers/{}/LogServices/Dump/",
                               BMCWEB_REDFISH_MANAGER_URI_NAME);
    }
    else
    {
        BMCWEB_LOG_ERROR("CreateDump failed. Unknown dump type");
        messages::internalError(asyncResp->res);
        return;
    }

    std::vector<std::pair<std::string, std::variant<std::string, uint64_t>>>
        createDumpParamVec;

    if (req.session != nullptr)
    {
        createDumpParamVec.emplace_back(
            "xyz.openbmc_project.Dump.Create.CreateParameters.OriginatorId",
            req.session->clientIp);
        createDumpParamVec.emplace_back(
            "xyz.openbmc_project.Dump.Create.CreateParameters.OriginatorType",
            "xyz.openbmc_project.Common.OriginatedBy.OriginatorTypes.Client");
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp, payload(task::Payload(req)),
         dumpPath](const boost::system::error_code& ec,
                   const sdbusplus::message_t& msg,
                   const sdbusplus::message::object_path& objPath) mutable {
        if (ec)
        {
            BMCWEB_LOG_ERROR("CreateDump resp_handler got error {}", ec);
            const sd_bus_error* dbusError = msg.get_error();
            if (dbusError == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            BMCWEB_LOG_ERROR("CreateDump DBus error: {} and error msg: {}",
                             dbusError->name, dbusError->message);
            if (std::string_view(
                    "xyz.openbmc_project.Common.Error.NotAllowed") ==
                dbusError->name)
            {
                messages::resourceInStandby(asyncResp->res);
                return;
            }
            if (std::string_view(
                    "xyz.openbmc_project.Dump.Create.Error.Disabled") ==
                dbusError->name)
            {
                messages::serviceDisabled(asyncResp->res, dumpPath);
                return;
            }
            if (std::string_view(
                    "xyz.openbmc_project.Common.Error.Unavailable") ==
                dbusError->name)
            {
                messages::resourceInUse(asyncResp->res);
                return;
            }
            // Other Dbus errors such as:
            // xyz.openbmc_project.Common.Error.InvalidArgument &
            // org.freedesktop.DBus.Error.InvalidArgs are all related to
            // the dbus call that is made here in the bmcweb
            // implementation and has nothing to do with the client's
            // input in the request. Hence, returning internal error
            // back to the client.
            messages::internalError(asyncResp->res);
            return;
        }
        BMCWEB_LOG_DEBUG("Dump Created. Path: {}", objPath.str);
        createDumpTaskCallback(std::move(payload), asyncResp, objPath);
    },
        "xyz.openbmc_project.Dump.Manager", getDumpPath(dumpType),
        "xyz.openbmc_project.Dump.Create", "CreateDump", createDumpParamVec);
}

inline void clearDump(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& dumpType)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code& ec) {
        if (ec)
        {
            BMCWEB_LOG_ERROR("clearDump resp_handler got error {}", ec);
            messages::internalError(asyncResp->res);
            return;
        }
    }, "xyz.openbmc_project.Dump.Manager", getDumpPath(dumpType),
        "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
}

inline void
    parseCrashdumpParameters(const dbus::utility::DBusPropertiesMap& params,
                             std::string& filename, std::string& timestamp,
                             std::string& logfile)
{
    const std::string* filenamePtr = nullptr;
    const std::string* timestampPtr = nullptr;
    const std::string* logfilePtr = nullptr;

    const bool success = sdbusplus::unpackPropertiesNoThrow(
        dbus_utils::UnpackErrorPrinter(), params, "Timestamp", timestampPtr,
        "Filename", filenamePtr, "Log", logfilePtr);

    if (!success)
    {
        return;
    }

    if (filenamePtr != nullptr)
    {
        filename = *filenamePtr;
    }

    if (timestampPtr != nullptr)
    {
        timestamp = *timestampPtr;
    }

    if (logfilePtr != nullptr)
    {
        logfile = *logfilePtr;
    }
}

inline void requestRoutesSystemLogServiceCollection(App& app)
{
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/")
        .privileges(redfish::privileges::getLogServiceCollection)
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

        // Collections don't include the static data added by SubRoute
        // because it has a duplicate entry for members
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogServiceCollection.LogServiceCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "System Log Services Collection";
        asyncResp->res.jsonValue["Description"] =
            "Collection of LogServices for this Computer System";
        nlohmann::json& logServiceArray = asyncResp->res.jsonValue["Members"];
        logServiceArray = nlohmann::json::array();
        nlohmann::json::object_t eventLog;
        eventLog["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/EventLog",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        logServiceArray.emplace_back(std::move(eventLog));
        if constexpr (BMCWEB_REDFISH_DUMP_LOG)
        {
            nlohmann::json::object_t dumpLog;
            dumpLog["@odata.id"] =
                std::format("/redfish/v1/Systems/{}/LogServices/Dump",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME);
            logServiceArray.emplace_back(std::move(dumpLog));
        }

        if constexpr (BMCWEB_REDFISH_CPU_LOG)
        {
            nlohmann::json::object_t crashdump;
            crashdump["@odata.id"] =
                std::format("/redfish/v1/Systems/{}/LogServices/Crashdump",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME);
            logServiceArray.emplace_back(std::move(crashdump));
        }

        if constexpr (BMCWEB_REDFISH_HOST_LOGGER)
        {
            nlohmann::json::object_t hostlogger;
            hostlogger["@odata.id"] =
                std::format("/redfish/v1/Systems/{}/LogServices/HostLogger",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME);
            logServiceArray.emplace_back(std::move(hostlogger));
        }
        asyncResp->res.jsonValue["Members@odata.count"] =
            logServiceArray.size();

        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.State.Boot.PostCode"};
        dbus::utility::getSubTreePaths(
            "/", 0, interfaces,
            [asyncResp](const boost::system::error_code& ec,
                        const dbus::utility::MapperGetSubTreePathsResponse&
                            subtreePath) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("{}", ec);
                return;
            }

            for (const auto& pathStr : subtreePath)
            {
                if (pathStr.find("PostCode") != std::string::npos)
                {
                    nlohmann::json& logServiceArrayLocal =
                        asyncResp->res.jsonValue["Members"];
                    nlohmann::json::object_t member;
                    member["@odata.id"] = std::format(
                        "/redfish/v1/Systems/{}/LogServices/PostCodes",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);

                    logServiceArrayLocal.emplace_back(std::move(member));

                    asyncResp->res.jsonValue["Members@odata.count"] =
                        logServiceArrayLocal.size();
                    return;
                }
            }
        });
    });
}

inline void requestRoutesEventLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/EventLog",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["Name"] = "Event Log Service";
        asyncResp->res.jsonValue["Description"] = "System Event Log Service";
        asyncResp->res.jsonValue["Id"] = "EventLog";
        asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

        std::pair<std::string, std::string> redfishDateTimeOffset =
            redfish::time_utils::getDateTimeOffsetNow();

        asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
        asyncResp->res.jsonValue["DateTimeLocalOffset"] =
            redfishDateTimeOffset.second;

        asyncResp->res.jsonValue["Entries"]["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/EventLog/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]["target"]

            = std::format(
                "/redfish/v1/Systems/{}/LogServices/EventLog/Actions/LogService.ClearLog",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
    });
}

inline void requestRoutesJournalEventLogClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/EventLog/Actions/LogService.ClearLog/")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
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
        // Clear the EventLog by deleting the log files
        std::vector<std::filesystem::path> redfishLogFiles;
        if (getRedfishLogFiles(redfishLogFiles))
        {
            for (const std::filesystem::path& file : redfishLogFiles)
            {
                std::error_code ec;
                std::filesystem::remove(file, ec);
            }
        }

        // Reload rsyslog so it knows to start new log files
        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code& ec) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("Failed to reload rsyslog: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            messages::success(asyncResp->res);
        }, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
            "org.freedesktop.systemd1.Manager", "ReloadUnit", "rsyslog.service",
            "replace");
    });
}

enum class LogParseError
{
    success,
    parseFailed,
    messageIdNotInRegistry,
};

static LogParseError
    fillEventLogEntryJson(const std::string& logEntryID,
                          const std::string& logEntry,
                          nlohmann::json::object_t& logEntryJson)
{
    // The redfish log format is "<Timestamp> <MessageId>,<MessageArgs>"
    // First get the Timestamp
    size_t space = logEntry.find_first_of(' ');
    if (space == std::string::npos)
    {
        return LogParseError::parseFailed;
    }
    std::string timestamp = logEntry.substr(0, space);
    // Then get the log contents
    size_t entryStart = logEntry.find_first_not_of(' ', space);
    if (entryStart == std::string::npos)
    {
        return LogParseError::parseFailed;
    }
    std::string_view entry(logEntry);
    entry.remove_prefix(entryStart);
    // Use split to separate the entry into its fields
    std::vector<std::string> logEntryFields;
    bmcweb::split(logEntryFields, entry, ',');
    // We need at least a MessageId to be valid
    auto logEntryIter = logEntryFields.begin();
    if (logEntryIter == logEntryFields.end())
    {
        return LogParseError::parseFailed;
    }
    std::string& messageID = *logEntryIter;
    // Get the Message from the MessageRegistry
    const registries::Message* message = registries::getMessage(messageID);

    logEntryIter++;
    if (message == nullptr)
    {
        BMCWEB_LOG_WARNING("Log entry not found in registry: {}", logEntry);
        return LogParseError::messageIdNotInRegistry;
    }

    std::vector<std::string_view> messageArgs(logEntryIter,
                                              logEntryFields.end());
    messageArgs.resize(message->numberOfArgs);

    std::string msg = redfish::registries::fillMessageArgs(messageArgs,
                                                           message->message);
    if (msg.empty())
    {
        return LogParseError::parseFailed;
    }

    // Get the Created time from the timestamp. The log timestamp is in RFC3339
    // format which matches the Redfish format except for the fractional seconds
    // between the '.' and the '+', so just remove them.
    std::size_t dot = timestamp.find_first_of('.');
    std::size_t plus = timestamp.find_first_of('+');
    if (dot != std::string::npos && plus != std::string::npos)
    {
        timestamp.erase(dot, plus - dot);
    }

    // Fill in the log entry with the gathered data
    logEntryJson["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    logEntryJson["@odata.id"] = boost::urls::format(
        "/redfish/v1/Systems/{}/LogServices/EventLog/Entries/{}",
        BMCWEB_REDFISH_SYSTEM_URI_NAME, logEntryID);
    logEntryJson["Name"] = "System Event Log Entry";
    logEntryJson["Id"] = logEntryID;
    logEntryJson["Message"] = std::move(msg);
    logEntryJson["MessageId"] = std::move(messageID);
    logEntryJson["MessageArgs"] = messageArgs;
    logEntryJson["EntryType"] = "Event";
    logEntryJson["Severity"] = message->messageSeverity;
    logEntryJson["Created"] = std::move(timestamp);
    return LogParseError::success;
}

std::string severityToString(int level)
{
    switch (level)
    {
        case 0:
            return "xyz.openbmc_project.Logging.Entry.Level.Emergency";
        case 1:
            return "xyz.openbmc_project.Logging.Entry.Level.Alert";
        case 2:
            return "xyz.openbmc_project.Logging.Entry.Level.Critical";
        case 3:
            return "xyz.openbmc_project.Logging.Entry.Level.Error";
        case 4:
            return "xyz.openbmc_project.Logging.Entry.Level.Warning";
        case 5:
            return "xyz.openbmc_project.Logging.Entry.Level.Notice";
        case 6:
            return "xyz.openbmc_project.Logging.Entry.Level.Informational";
        case 7:
            return "xyz.openbmc_project.Logging.Entry.Level.Debug";
        default:
            return "Unknown";
    }
}

inline void requestRoutesEventLogEntriesPost(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/EventLog/Actions/Oem/OpenBMC.LogService.CreateLogEntry")
        .privileges(redfish::privileges::postLogEntry)
        .methods(boost::beast::http::verb::post)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {

        BMCWEB_LOG_DEBUG("EventLog POST called");
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        // Parse request body
        std::string message;
        std::string severity;
        int severityNumber;
        std::map<std::string, std::string> additionalData;
        nlohmann::json json = nlohmann::json::parse(req.body(), nullptr, false);
        // Required: Message, Severity, AdditionalData
        if (!json.contains("Message") || !json.contains("Severity"))
        {
            messages::propertyMissing(asyncResp->res, "Message or Severity");
            return;
        }

        message = json.at("Message").get<std::string>();
        severityNumber = json.at("Severity").get<int>();
        severity = severityToString(severityNumber);

        BMCWEB_LOG_DEBUG("event log entry message :{}  and severity {}",
                         message, severity);
        if (json.contains("AdditionalData"))
        {
            if (!json["AdditionalData"].is_object())
            {
                messages::propertyValueTypeError(asyncResp->res,
                                                 "AdditionalData", "object");
                return;
            }

            for (const auto& [key, value] : json["AdditionalData"].items())
            {
                try
                {
                    additionalData[key] = value.get<std::string>();
                    BMCWEB_LOG_ERROR("event log entry additonal :{} ",
                                     additionalData[key]);
                }
                catch (const nlohmann::json::type_error&)
                {
                    messages::propertyValueTypeError(
                        asyncResp->res, "AdditionalData." + key, "string");
                    return;
                }
            }
        }

        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code ec) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("Failed to create log entry: {}",
                                 ec.message());
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
        }, "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create", message, severity,
            additionalData);
    });
}

inline void requestRoutesJournalEventLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
        query_param::QueryCapabilities capabilities = {
            .canDelegateTop = true,
            .canDelegateSkip = true,
        };
        query_param::Query delegatedQuery;
        if (!redfish::setUpRedfishRouteWithDelegation(
                app, req, asyncResp, delegatedQuery, capabilities))
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

        size_t top = delegatedQuery.top.value_or(query_param::Query::maxTop);
        size_t skip = delegatedQuery.skip.value_or(0);

        // Collections don't include the static data added by SubRoute
        // because it has a duplicate entry for members
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/EventLog/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "System Event Log Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of System Event Log Entries";

        nlohmann::json& logEntryArray = asyncResp->res.jsonValue["Members"];
        logEntryArray = nlohmann::json::array();
        // Go through the log files and create a unique ID for each
        // entry
        std::vector<std::filesystem::path> redfishLogFiles;
        getRedfishLogFiles(redfishLogFiles);
        uint64_t entryCount = 0;
        std::string logEntry;

        // Oldest logs are in the last file, so start there and loop
        // backwards
        for (auto it = redfishLogFiles.rbegin(); it < redfishLogFiles.rend();
             it++)
        {
            std::ifstream logStream(*it);
            if (!logStream.is_open())
            {
                continue;
            }

            // Reset the unique ID on the first entry
            bool firstEntry = true;
            while (std::getline(logStream, logEntry))
            {
                std::string idStr;
                if (!getUniqueEntryID(logEntry, idStr, firstEntry))
                {
                    continue;
                }
                firstEntry = false;

                nlohmann::json::object_t bmcLogEntry;
                LogParseError status = fillEventLogEntryJson(idStr, logEntry,
                                                             bmcLogEntry);
                if (status == LogParseError::messageIdNotInRegistry)
                {
                    continue;
                }
                if (status != LogParseError::success)
                {
                    messages::internalError(asyncResp->res);
                    return;
                }

                entryCount++;
                // Handle paging using skip (number of entries to skip from the
                // start) and top (number of entries to display)
                if (entryCount <= skip || entryCount > skip + top)
                {
                    continue;
                }

                logEntryArray.emplace_back(std::move(bmcLogEntry));
            }
        }
        asyncResp->res.jsonValue["Members@odata.count"] = entryCount;
        if (skip + top < entryCount)
        {
            asyncResp->res
                .jsonValue["Members@odata.nextLink"] = boost::urls::format(
                "/redfish/v1/Systems/{}/LogServices/EventLog/Entries?$skip={}",
                BMCWEB_REDFISH_SYSTEM_URI_NAME, std::to_string(skip + top));
        }
    });
}

inline void requestRoutesJournalEventLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
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

        const std::string& targetID = param;

        // Go through the log files and check the unique ID for each
        // entry to find the target entry
        std::vector<std::filesystem::path> redfishLogFiles;
        getRedfishLogFiles(redfishLogFiles);
        std::string logEntry;

        // Oldest logs are in the last file, so start there and loop
        // backwards
        for (auto it = redfishLogFiles.rbegin(); it < redfishLogFiles.rend();
             it++)
        {
            std::ifstream logStream(*it);
            if (!logStream.is_open())
            {
                continue;
            }

            // Reset the unique ID on the first entry
            bool firstEntry = true;
            while (std::getline(logStream, logEntry))
            {
                std::string idStr;
                if (!getUniqueEntryID(logEntry, idStr, firstEntry))
                {
                    continue;
                }
                firstEntry = false;

                if (idStr == targetID)
                {
                    nlohmann::json::object_t bmcLogEntry;
                    LogParseError status =
                        fillEventLogEntryJson(idStr, logEntry, bmcLogEntry);
                    if (status != LogParseError::success)
                    {
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    asyncResp->res.jsonValue.update(bmcLogEntry);
                    return;
                }
            }
        }
        // Requested ID was not found
        messages::resourceNotFound(asyncResp->res, "LogEntry", targetID);
    });
}

inline void requestRoutesDBusEventLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
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

        // Collections don't include the static data added by SubRoute
        // because it has a duplicate entry for members
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/EventLog/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "System Event Log Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of System Event Log Entries";

        // DBus implementation of EventLog/Entries
        // Make call to Logging Service to find all log entry objects
        sdbusplus::message::object_path path("/xyz/openbmc_project/logging");
        dbus::utility::getManagedObjects(
            "xyz.openbmc_project.Logging", path,
            [asyncResp](const boost::system::error_code& ec,
                        const dbus::utility::ManagedObjectType& resp) {
            if (ec)
            {
                // TODO Handle for specific error code
                BMCWEB_LOG_ERROR(
                    "getLogEntriesIfaceData resp_handler got error {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            nlohmann::json::array_t entriesArray;
            for (const auto& objectPath : resp)
            {
                const uint32_t* id = nullptr;
                const uint64_t* timestamp = nullptr;
                const uint64_t* updateTimestamp = nullptr;
                const std::string* severity = nullptr;
                const std::string* message = nullptr;
                const std::string* filePath = nullptr;
                const std::string* resolution = nullptr;
                bool resolved = false;
                const std::string* notify = nullptr;

                for (const auto& interfaceMap : objectPath.second)
                {
                    if (interfaceMap.first ==
                        "xyz.openbmc_project.Logging.Entry")
                    {
                        for (const auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Id")
                            {
                                id = std::get_if<uint32_t>(&propertyMap.second);
                            }
                            else if (propertyMap.first == "Timestamp")
                            {
                                timestamp =
                                    std::get_if<uint64_t>(&propertyMap.second);
                            }
                            else if (propertyMap.first == "UpdateTimestamp")
                            {
                                updateTimestamp =
                                    std::get_if<uint64_t>(&propertyMap.second);
                            }
                            else if (propertyMap.first == "Severity")
                            {
                                severity = std::get_if<std::string>(
                                    &propertyMap.second);
                            }
                            else if (propertyMap.first == "Resolution")
                            {
                                resolution = std::get_if<std::string>(
                                    &propertyMap.second);
                            }
                            else if (propertyMap.first == "Message")
                            {
                                message = std::get_if<std::string>(
                                    &propertyMap.second);
                            }
                            else if (propertyMap.first == "Resolved")
                            {
                                const bool* resolveptr =
                                    std::get_if<bool>(&propertyMap.second);
                                if (resolveptr == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    return;
                                }
                                resolved = *resolveptr;
                            }
                            else if (propertyMap.first ==
                                     "ServiceProviderNotify")
                            {
                                notify = std::get_if<std::string>(
                                    &propertyMap.second);
                                if (notify == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    return;
                                }
                            }
                        }
                        if (id == nullptr || message == nullptr ||
                            severity == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Common.FilePath")
                    {
                        for (const auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Path")
                            {
                                filePath = std::get_if<std::string>(
                                    &propertyMap.second);
                            }
                        }
                    }
                }
                // Object path without the
                // xyz.openbmc_project.Logging.Entry interface, ignore
                // and continue.
                if (id == nullptr || message == nullptr ||
                    severity == nullptr || timestamp == nullptr ||
                    updateTimestamp == nullptr)
                {
                    continue;
                }
                nlohmann::json& thisEntry = entriesArray.emplace_back();
                thisEntry["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
                thisEntry["@odata.id"] = boost::urls::format(
                    "/redfish/v1/Systems/{}/LogServices/EventLog/Entries/{}",
                    BMCWEB_REDFISH_SYSTEM_URI_NAME, std::to_string(*id));
                thisEntry["Name"] = "System Event Log Entry";
                thisEntry["Id"] = std::to_string(*id);
                thisEntry["Message"] = *message;
                thisEntry["Resolved"] = resolved;
                if ((resolution != nullptr) && (!(*resolution).empty()))
                {
                    thisEntry["Resolution"] = *resolution;
                }
                std::optional<bool> notifyAction =
                    getProviderNotifyAction(*notify);
                if (notifyAction)
                {
                    thisEntry["ServiceProviderNotified"] = *notifyAction;
                }
                thisEntry["EntryType"] = "Event";
                thisEntry["Severity"] =
                    translateSeverityDbusToRedfish(*severity);
                thisEntry["Created"] =
                    redfish::time_utils::getDateTimeUintMs(*timestamp);
                thisEntry["Modified"] =
                    redfish::time_utils::getDateTimeUintMs(*updateTimestamp);
                if (filePath != nullptr)
                {
                    thisEntry["AdditionalDataURI"] =
                        std::format(
                            "/redfish/v1/Systems/{}/LogServices/EventLog/Entries/",
                            BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                        std::to_string(*id) + "/attachment";
                }
            }
            std::ranges::sort(entriesArray, [](const nlohmann::json& left,
                                               const nlohmann::json& right) {
                return (left["Id"] <= right["Id"]);
            });
            asyncResp->res.jsonValue["Members@odata.count"] =
                entriesArray.size();
            asyncResp->res.jsonValue["Members"] = std::move(entriesArray);
        });
    });
}

inline void requestRoutesDBusEventLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
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

        std::string entryID = param;
        dbus::utility::escapePathForDbus(entryID);

        // DBus implementation of EventLog/Entries
        // Make call to Logging Service to find all log entry objects
        sdbusplus::asio::getAllProperties(
            *crow::connections::systemBus, "xyz.openbmc_project.Logging",
            "/xyz/openbmc_project/logging/entry/" + entryID, "",
            [asyncResp, entryID](const boost::system::error_code& ec,
                                 const dbus::utility::DBusPropertiesMap& resp) {
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "EventLogEntry",
                                           entryID);
                return;
            }
            if (ec)
            {
                BMCWEB_LOG_ERROR(
                    "EventLogEntry (DBus) resp_handler got error {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            const uint32_t* id = nullptr;
            const uint64_t* timestamp = nullptr;
            const uint64_t* updateTimestamp = nullptr;
            const std::string* severity = nullptr;
            const std::string* message = nullptr;
            const std::string* filePath = nullptr;
            const std::string* resolution = nullptr;
            bool resolved = false;
            const std::string* notify = nullptr;

            const bool success = sdbusplus::unpackPropertiesNoThrow(
                dbus_utils::UnpackErrorPrinter(), resp, "Id", id, "Timestamp",
                timestamp, "UpdateTimestamp", updateTimestamp, "Severity",
                severity, "Message", message, "Resolved", resolved,
                "Resolution", resolution, "Path", filePath,
                "ServiceProviderNotify", notify);

            if (!success)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            if (id == nullptr || message == nullptr || severity == nullptr ||
                timestamp == nullptr || updateTimestamp == nullptr ||
                notify == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntry.v1_9_0.LogEntry";
            asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
                "/redfish/v1/Systems/{}/LogServices/EventLog/Entries/{}",
                BMCWEB_REDFISH_SYSTEM_URI_NAME, std::to_string(*id));
            asyncResp->res.jsonValue["Name"] = "System Event Log Entry";
            asyncResp->res.jsonValue["Id"] = std::to_string(*id);
            asyncResp->res.jsonValue["Message"] = *message;
            asyncResp->res.jsonValue["Resolved"] = resolved;
            std::optional<bool> notifyAction = getProviderNotifyAction(*notify);
            if (notifyAction)
            {
                asyncResp->res.jsonValue["ServiceProviderNotified"] =
                    *notifyAction;
            }
            if ((resolution != nullptr) && (!(*resolution).empty()))
            {
                asyncResp->res.jsonValue["Resolution"] = *resolution;
            }
            asyncResp->res.jsonValue["EntryType"] = "Event";
            asyncResp->res.jsonValue["Severity"] =
                translateSeverityDbusToRedfish(*severity);
            asyncResp->res.jsonValue["Created"] =
                redfish::time_utils::getDateTimeUintMs(*timestamp);
            asyncResp->res.jsonValue["Modified"] =
                redfish::time_utils::getDateTimeUintMs(*updateTimestamp);
            if (filePath != nullptr)
            {
                asyncResp->res.jsonValue["AdditionalDataURI"] =
                    std::format(
                        "/redfish/v1/Systems/{}/LogServices/EventLog/Entries/",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                    std::to_string(*id) + "/attachment";
            }
        });
    });

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::patchLogEntry)
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& entryId) {
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
        std::optional<bool> resolved;

        if (!json_util::readJsonPatch(req, asyncResp->res, "Resolved",
                                      resolved))
        {
            return;
        }
        BMCWEB_LOG_DEBUG("Set Resolved");

        setDbusProperty(asyncResp, "Resolved", "xyz.openbmc_project.Logging",
                        "/xyz/openbmc_project/logging/entry/" + entryId,
                        "xyz.openbmc_project.Logging.Entry", "Resolved",
                        *resolved);
    });

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)

        .methods(boost::beast::http::verb::delete_)(
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
        BMCWEB_LOG_DEBUG("Do delete single event entries.");

        std::string entryID = param;

        dbus::utility::escapePathForDbus(entryID);

        // Process response from Logging service.
        auto respHandler = [asyncResp,
                            entryID](const boost::system::error_code& ec) {
            BMCWEB_LOG_DEBUG("EventLogEntry (DBus) doDelete callback: Done");
            if (ec)
            {
                if (ec.value() == EBADR)
                {
                    messages::resourceNotFound(asyncResp->res, "LogEntry",
                                               entryID);
                    return;
                }
                // TODO Handle for specific error code
                BMCWEB_LOG_ERROR(
                    "EventLogEntry (DBus) doDelete respHandler got error {}",
                    ec);
                asyncResp->res.result(
                    boost::beast::http::status::internal_server_error);
                return;
            }

            asyncResp->res.result(boost::beast::http::status::ok);
        };

        // Make call to Logging service to request Delete Log
        crow::connections::systemBus->async_method_call(
            respHandler, "xyz.openbmc_project.Logging",
            "/xyz/openbmc_project/logging/entry/" + entryID,
            "xyz.openbmc_project.Object.Delete", "Delete");
    });
}

constexpr const char* hostLoggerFolderPath = "/var/log/console";

inline bool
    getHostLoggerFiles(const std::string& hostLoggerFilePath,
                       std::vector<std::filesystem::path>& hostLoggerFiles)
{
    std::error_code ec;
    std::filesystem::directory_iterator logPath(hostLoggerFilePath, ec);
    if (ec)
    {
        BMCWEB_LOG_WARNING("{}", ec.message());
        return false;
    }
    for (const std::filesystem::directory_entry& it : logPath)
    {
        std::string filename = it.path().filename();
        // Prefix of each log files is "log". Find the file and save the
        // path
        if (filename.starts_with("log"))
        {
            hostLoggerFiles.emplace_back(it.path());
        }
    }
    // As the log files rotate, they are appended with a ".#" that is higher for
    // the older logs. Since we start from oldest logs, sort the name in
    // descending order.
    std::sort(hostLoggerFiles.rbegin(), hostLoggerFiles.rend(),
              AlphanumLess<std::string>());

    return true;
}

inline bool getHostLoggerEntries(
    const std::vector<std::filesystem::path>& hostLoggerFiles, uint64_t skip,
    uint64_t top, std::vector<std::string>& logEntries, size_t& logCount)
{
    GzFileReader logFile;

    // Go though all log files and expose host logs.
    for (const std::filesystem::path& it : hostLoggerFiles)
    {
        if (!logFile.gzGetLines(it.string(), skip, top, logEntries, logCount))
        {
            BMCWEB_LOG_ERROR("fail to expose host logs");
            return false;
        }
    }
    // Get lastMessage from constructor by getter
    std::string lastMessage = logFile.getLastMessage();
    if (!lastMessage.empty())
    {
        logCount++;
        if (logCount > skip && logCount <= (skip + top))
        {
            logEntries.push_back(lastMessage);
        }
    }
    return true;
}

inline void fillHostLoggerEntryJson(std::string_view logEntryID,
                                    std::string_view msg,
                                    nlohmann::json::object_t& logEntryJson)
{
    // Fill in the log entry with the gathered data.
    logEntryJson["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    logEntryJson["@odata.id"] = boost::urls::format(
        "/redfish/v1/Systems/{}/LogServices/HostLogger/Entries/{}",
        BMCWEB_REDFISH_SYSTEM_URI_NAME, logEntryID);
    logEntryJson["Name"] = "Host Logger Entry";
    logEntryJson["Id"] = logEntryID;
    logEntryJson["Message"] = msg;
    logEntryJson["EntryType"] = "Oem";
    logEntryJson["Severity"] = "OK";
    logEntryJson["OemRecordFormat"] = "Host Logger Entry";
}

inline void requestRoutesSystemHostLogger(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/HostLogger/")
        .privileges(redfish::privileges::getLogService)
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
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/HostLogger",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["Name"] = "Host Logger Service";
        asyncResp->res.jsonValue["Description"] = "Host Logger Service";
        asyncResp->res.jsonValue["Id"] = "HostLogger";
        asyncResp->res.jsonValue["Entries"]["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/HostLogger/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
    });
}

inline void requestRoutesSystemHostLoggerCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/HostLogger/Entries/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
        query_param::QueryCapabilities capabilities = {
            .canDelegateTop = true,
            .canDelegateSkip = true,
        };
        query_param::Query delegatedQuery;
        if (!redfish::setUpRedfishRouteWithDelegation(
                app, req, asyncResp, delegatedQuery, capabilities))
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
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/HostLogger/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["Name"] = "HostLogger Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of HostLogger Entries";
        nlohmann::json& logEntryArray = asyncResp->res.jsonValue["Members"];
        logEntryArray = nlohmann::json::array();
        asyncResp->res.jsonValue["Members@odata.count"] = 0;

        std::vector<std::filesystem::path> hostLoggerFiles;
        if (!getHostLoggerFiles(hostLoggerFolderPath, hostLoggerFiles))
        {
            BMCWEB_LOG_DEBUG("Failed to get host log file path");
            return;
        }
        // If we weren't provided top and skip limits, use the defaults.
        size_t skip = delegatedQuery.skip.value_or(0);
        size_t top = delegatedQuery.top.value_or(query_param::Query::maxTop);
        size_t logCount = 0;
        // This vector only store the entries we want to expose that
        // control by skip and top.
        std::vector<std::string> logEntries;
        if (!getHostLoggerEntries(hostLoggerFiles, skip, top, logEntries,
                                  logCount))
        {
            messages::internalError(asyncResp->res);
            return;
        }
        // If vector is empty, that means skip value larger than total
        // log count
        if (logEntries.empty())
        {
            asyncResp->res.jsonValue["Members@odata.count"] = logCount;
            return;
        }
        if (!logEntries.empty())
        {
            for (size_t i = 0; i < logEntries.size(); i++)
            {
                nlohmann::json::object_t hostLogEntry;
                fillHostLoggerEntryJson(std::to_string(skip + i), logEntries[i],
                                        hostLogEntry);
                logEntryArray.emplace_back(std::move(hostLogEntry));
            }

            asyncResp->res.jsonValue["Members@odata.count"] = logCount;
            if (skip + top < logCount)
            {
                asyncResp->res.jsonValue["Members@odata.nextLink"] =
                    std::format(
                        "/redfish/v1/Systems/{}/LogServices/HostLogger/Entries?$skip=",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                    std::to_string(skip + top);
            }
        }
    });
}

inline void requestRoutesSystemHostLoggerLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/HostLogger/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
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
        std::string_view targetID = param;

        uint64_t idInt = 0;

        auto [ptr, ec] = std::from_chars(targetID.begin(), targetID.end(),
                                         idInt);
        if (ec != std::errc{} || ptr != targetID.end())
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", param);
            return;
        }

        std::vector<std::filesystem::path> hostLoggerFiles;
        if (!getHostLoggerFiles(hostLoggerFolderPath, hostLoggerFiles))
        {
            BMCWEB_LOG_DEBUG("Failed to get host log file path");
            return;
        }

        size_t logCount = 0;
        size_t top = 1;
        std::vector<std::string> logEntries;
        // We can get specific entry by skip and top. For example, if we
        // want to get nth entry, we can set skip = n-1 and top = 1 to
        // get that entry
        if (!getHostLoggerEntries(hostLoggerFiles, idInt, top, logEntries,
                                  logCount))
        {
            messages::internalError(asyncResp->res);
            return;
        }

        if (!logEntries.empty())
        {
            nlohmann::json::object_t hostLogEntry;
            fillHostLoggerEntryJson(targetID, logEntries[0], hostLogEntry);
            asyncResp->res.jsonValue.update(hostLogEntry);
            return;
        }

        // Requested ID was not found
        messages::resourceNotFound(asyncResp->res, "LogEntry", param);
    });
}

inline void handleBMCLogServicesCollectionGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    // Collections don't include the static data added by SubRoute
    // because it has a duplicate entry for members
    asyncResp->res.jsonValue["@odata.type"] =
        "#LogServiceCollection.LogServiceCollection";
    asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
        "/redfish/v1/Managers/{}/LogServices", BMCWEB_REDFISH_MANAGER_URI_NAME);
    asyncResp->res.jsonValue["Name"] = "Open BMC Log Services Collection";
    asyncResp->res.jsonValue["Description"] =
        "Collection of LogServices for this Manager";
    nlohmann::json& logServiceArray = asyncResp->res.jsonValue["Members"];
    logServiceArray = nlohmann::json::array();

    if constexpr (BMCWEB_REDFISH_BMC_JOURNAL)
    {
        nlohmann::json::object_t journal;
        journal["@odata.id"] =
            boost::urls::format("/redfish/v1/Managers/{}/LogServices/Journal",
                                BMCWEB_REDFISH_MANAGER_URI_NAME);
        logServiceArray.emplace_back(std::move(journal));
    }

    asyncResp->res.jsonValue["Members@odata.count"] = logServiceArray.size();

    if constexpr (BMCWEB_REDFISH_DUMP_LOG)
    {
        constexpr std::array<std::string_view, 1> interfaces = {
            "xyz.openbmc_project.Collection.DeleteAll"};
        dbus::utility::getSubTreePaths(
            "/xyz/openbmc_project/dump", 0, interfaces,
            [asyncResp](const boost::system::error_code& ec,
                        const dbus::utility::MapperGetSubTreePathsResponse&
                            subTreePaths) {
            if (ec)
            {
                BMCWEB_LOG_ERROR(
                    "handleBMCLogServicesCollectionGet respHandler got error {}",
                    ec);
                // Assume that getting an error simply means there are no dump
                // LogServices. Return without adding any error response.
                return;
            }

            nlohmann::json& logServiceArrayLocal =
                asyncResp->res.jsonValue["Members"];

            for (const std::string& path : subTreePaths)
            {
                if (path == "/xyz/openbmc_project/dump/bmc")
                {
                    nlohmann::json::object_t member;
                    member["@odata.id"] = boost::urls::format(
                        "/redfish/v1/Managers/{}/LogServices/Dump",
                        BMCWEB_REDFISH_MANAGER_URI_NAME);
                    logServiceArrayLocal.emplace_back(std::move(member));
                }
                else if (path == "/xyz/openbmc_project/dump/faultlog")
                {
                    nlohmann::json::object_t member;
                    member["@odata.id"] = boost::urls::format(
                        "/redfish/v1/Managers/{}/LogServices/FaultLog",
                        BMCWEB_REDFISH_MANAGER_URI_NAME);
                    logServiceArrayLocal.emplace_back(std::move(member));
                }
            }

            asyncResp->res.jsonValue["Members@odata.count"] =
                logServiceArrayLocal.size();
        });
    }
}

inline void requestRoutesBMCLogServiceCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/")
        .privileges(redfish::privileges::getLogServiceCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleBMCLogServicesCollectionGet, std::ref(app)));
}

inline void requestRoutesBMCJournalLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/Journal/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& managerId) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "Manager", managerId);
            return;
        }

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["@odata.id"] =
            boost::urls::format("/redfish/v1/Managers/{}/LogServices/Journal",
                                BMCWEB_REDFISH_MANAGER_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "Open BMC Journal Log Service";
        asyncResp->res.jsonValue["Description"] = "BMC Journal Log Service";
        asyncResp->res.jsonValue["Id"] = "Journal";
        asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

        std::pair<std::string, std::string> redfishDateTimeOffset =
            redfish::time_utils::getDateTimeOffsetNow();
        asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
        asyncResp->res.jsonValue["DateTimeLocalOffset"] =
            redfishDateTimeOffset.second;

        asyncResp->res.jsonValue["Entries"]["@odata.id"] = boost::urls::format(
            "/redfish/v1/Managers/{}/LogServices/Journal/Entries",
            BMCWEB_REDFISH_MANAGER_URI_NAME);
    });
}

static int
    fillBMCJournalLogEntryJson(const std::string& bmcJournalLogEntryID,
                               sd_journal* journal,
                               nlohmann::json::object_t& bmcJournalLogEntryJson)
{
    // Get the Log Entry contents
    int ret = 0;

    std::string message;
    std::string_view syslogID;
    ret = getJournalMetadata(journal, "SYSLOG_IDENTIFIER", syslogID);
    if (ret < 0)
    {
        BMCWEB_LOG_DEBUG("Failed to read SYSLOG_IDENTIFIER field: {}",
                         strerror(-ret));
    }
    if (!syslogID.empty())
    {
        message += std::string(syslogID) + ": ";
    }

    std::string_view msg;
    ret = getJournalMetadata(journal, "MESSAGE", msg);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR("Failed to read MESSAGE field: {}", strerror(-ret));
        return 1;
    }
    message += std::string(msg);

    // Get the severity from the PRIORITY field
    long int severity = 8; // Default to an invalid priority
    ret = getJournalMetadata(journal, "PRIORITY", 10, severity);
    if (ret < 0)
    {
        BMCWEB_LOG_DEBUG("Failed to read PRIORITY field: {}", strerror(-ret));
    }

    // Get the Created time from the timestamp
    std::string entryTimeStr;
    if (!getEntryTimestamp(journal, entryTimeStr))
    {
        return 1;
    }

    // Fill in the log entry with the gathered data
    bmcJournalLogEntryJson["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    bmcJournalLogEntryJson["@odata.id"] = boost::urls::format(
        "/redfish/v1/Managers/{}/LogServices/Journal/Entries/{}",
        BMCWEB_REDFISH_MANAGER_URI_NAME, bmcJournalLogEntryID);
    bmcJournalLogEntryJson["Name"] = "BMC Journal Entry";
    bmcJournalLogEntryJson["Id"] = bmcJournalLogEntryID;
    bmcJournalLogEntryJson["Message"] = std::move(message);
    bmcJournalLogEntryJson["EntryType"] = "Oem";
    log_entry::EventSeverity severityEnum = log_entry::EventSeverity::OK;
    if (severity <= 2)
    {
        severityEnum = log_entry::EventSeverity::Critical;
    }
    else if (severity <= 4)
    {
        severityEnum = log_entry::EventSeverity::Warning;
    }

    bmcJournalLogEntryJson["Severity"] = severityEnum;
    bmcJournalLogEntryJson["OemRecordFormat"] = "BMC Journal Entry";
    bmcJournalLogEntryJson["Created"] = std::move(entryTimeStr);
    return 0;
}

inline void requestRoutesBMCJournalLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/Journal/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& managerId) {
        query_param::QueryCapabilities capabilities = {
            .canDelegateTop = true,
            .canDelegateSkip = true,
        };
        query_param::Query delegatedQuery;
        if (!redfish::setUpRedfishRouteWithDelegation(
                app, req, asyncResp, delegatedQuery, capabilities))
        {
            return;
        }

        if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "Manager", managerId);
            return;
        }

        size_t skip = delegatedQuery.skip.value_or(0);
        size_t top = delegatedQuery.top.value_or(query_param::Query::maxTop);

        // Collections don't include the static data added by SubRoute
        // because it has a duplicate entry for members
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
            "/redfish/v1/Managers/{}/LogServices/Journal/Entries",
            BMCWEB_REDFISH_MANAGER_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "Open BMC Journal Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of BMC Journal Entries";
        nlohmann::json& logEntryArray = asyncResp->res.jsonValue["Members"];
        logEntryArray = nlohmann::json::array();

        // Go through the journal and use the timestamp to create a
        // unique ID for each entry
        sd_journal* journalTmp = nullptr;
        int ret = sd_journal_open(&journalTmp, SD_JOURNAL_LOCAL_ONLY);
        if (ret < 0)
        {
            BMCWEB_LOG_ERROR("failed to open journal: {}", strerror(-ret));
            messages::internalError(asyncResp->res);
            return;
        }
        std::unique_ptr<sd_journal, decltype(&sd_journal_close)> journal(
            journalTmp, sd_journal_close);
        journalTmp = nullptr;
        uint64_t entryCount = 0;
        // Reset the unique ID on the first entry
        bool firstEntry = true;
        SD_JOURNAL_FOREACH(journal.get())
        {
            entryCount++;
            // Handle paging using skip (number of entries to skip from
            // the start) and top (number of entries to display)
            if (entryCount <= skip || entryCount > skip + top)
            {
                continue;
            }

            std::string idStr;
            if (!getUniqueEntryID(journal.get(), idStr, firstEntry))
            {
                continue;
            }
            firstEntry = false;

            nlohmann::json::object_t bmcJournalLogEntry;
            if (fillBMCJournalLogEntryJson(idStr, journal.get(),
                                           bmcJournalLogEntry) != 0)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            logEntryArray.emplace_back(std::move(bmcJournalLogEntry));
        }
        asyncResp->res.jsonValue["Members@odata.count"] = entryCount;
        if (skip + top < entryCount)
        {
            asyncResp->res
                .jsonValue["Members@odata.nextLink"] = boost::urls::format(
                "/redfish/v1/Managers/{}/LogServices/Journal/Entries?$skip={}",
                BMCWEB_REDFISH_MANAGER_URI_NAME, std::to_string(skip + top));
        }
    });
}

inline void requestRoutesBMCJournalLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/LogServices/Journal/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& managerId, const std::string& entryID) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }

        if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "Manager", managerId);
            return;
        }

        // Convert the unique ID back to a timestamp to find the entry
        sd_id128_t bootID{};
        uint64_t ts = 0;
        uint64_t index = 0;
        if (!getTimestampFromID(asyncResp, entryID, bootID, ts, index))
        {
            return;
        }

        sd_journal* journalTmp = nullptr;
        int ret = sd_journal_open(&journalTmp, SD_JOURNAL_LOCAL_ONLY);
        if (ret < 0)
        {
            BMCWEB_LOG_ERROR("failed to open journal: {}", strerror(-ret));
            messages::internalError(asyncResp->res);
            return;
        }
        std::unique_ptr<sd_journal, decltype(&sd_journal_close)> journal(
            journalTmp, sd_journal_close);
        journalTmp = nullptr;
        // Go to the timestamp in the log and move to the entry at the
        // index tracking the unique ID
        std::string idStr;
        bool firstEntry = true;
        ret = sd_journal_seek_monotonic_usec(journal.get(), bootID, ts);
        if (ret < 0)
        {
            BMCWEB_LOG_ERROR("failed to seek to an entry in journal{}",
                             strerror(-ret));
            messages::internalError(asyncResp->res);
            return;
        }
        for (uint64_t i = 0; i <= index; i++)
        {
            sd_journal_next(journal.get());
            if (!getUniqueEntryID(journal.get(), idStr, firstEntry))
            {
                messages::internalError(asyncResp->res);
                return;
            }
            firstEntry = false;
        }
        // Confirm that the entry ID matches what was requested
        if (idStr != entryID)
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", entryID);
            return;
        }

        nlohmann::json::object_t bmcJournalLogEntry;
        if (fillBMCJournalLogEntryJson(entryID, journal.get(),
                                       bmcJournalLogEntry) != 0)
        {
            messages::internalError(asyncResp->res);
            return;
        }
        asyncResp->res.jsonValue.update(bmcJournalLogEntry);
    });
}

inline void
    getDumpServiceInfo(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& dumpType)
{
    std::string dumpPath;
    std::string overWritePolicy;
    bool collectDiagnosticDataSupported = false;

    if (dumpType == "BMC")
    {
        dumpPath = std::format("/redfish/v1/Managers/{}/LogServices/Dump",
                               BMCWEB_REDFISH_MANAGER_URI_NAME);
        overWritePolicy = "WrapsWhenFull";
        collectDiagnosticDataSupported = true;
    }
    else if (dumpType == "FaultLog")
    {
        dumpPath = std::format("/redfish/v1/Managers/{}/LogServices/FaultLog",
                               BMCWEB_REDFISH_MANAGER_URI_NAME);
        overWritePolicy = "Unknown";
        collectDiagnosticDataSupported = false;
    }
    else if (dumpType == "System")
    {
        dumpPath = std::format("/redfish/v1/Systems/{}/LogServices/Dump",
                               BMCWEB_REDFISH_SYSTEM_URI_NAME);
        overWritePolicy = "WrapsWhenFull";
        collectDiagnosticDataSupported = true;
    }
    else
    {
        BMCWEB_LOG_ERROR("getDumpServiceInfo() invalid dump type: {}",
                         dumpType);
        messages::internalError(asyncResp->res);
        return;
    }

    asyncResp->res.jsonValue["@odata.id"] = dumpPath;
    asyncResp->res.jsonValue["@odata.type"] = "#LogService.v1_2_0.LogService";
    asyncResp->res.jsonValue["Name"] = "Dump LogService";
    asyncResp->res.jsonValue["Description"] = dumpType + " Dump LogService";
    asyncResp->res.jsonValue["Id"] = std::filesystem::path(dumpPath).filename();
    asyncResp->res.jsonValue["OverWritePolicy"] = std::move(overWritePolicy);

    std::pair<std::string, std::string> redfishDateTimeOffset =
        redfish::time_utils::getDateTimeOffsetNow();
    asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
    asyncResp->res.jsonValue["DateTimeLocalOffset"] =
        redfishDateTimeOffset.second;

    asyncResp->res.jsonValue["Entries"]["@odata.id"] = dumpPath + "/Entries";

    if (collectDiagnosticDataSupported)
    {
        asyncResp->res.jsonValue["Actions"]["#LogService.CollectDiagnosticData"]
                                ["target"] =
            dumpPath + "/Actions/LogService.CollectDiagnosticData";
    }

    constexpr std::array<std::string_view, 1> interfaces = {deleteAllInterface};
    dbus::utility::getSubTreePaths(
        "/xyz/openbmc_project/dump", 0, interfaces,
        [asyncResp, dumpType, dumpPath](
            const boost::system::error_code& ec,
            const dbus::utility::MapperGetSubTreePathsResponse& subTreePaths) {
        if (ec)
        {
            BMCWEB_LOG_ERROR("getDumpServiceInfo respHandler got error {}", ec);
            // Assume that getting an error simply means there are no dump
            // LogServices. Return without adding any error response.
            return;
        }
        std::string dbusDumpPath = getDumpPath(dumpType);
        for (const std::string& path : subTreePaths)
        {
            if (path == dbusDumpPath)
            {
                asyncResp->res
                    .jsonValue["Actions"]["#LogService.ClearLog"]["target"] =
                    dumpPath + "/Actions/LogService.ClearLog";
                break;
            }
        }
    });
}

inline void handleLogServicesDumpServiceGet(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    getDumpServiceInfo(asyncResp, dumpType);
}

inline void handleLogServicesDumpServiceComputerSystemGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (chassisId != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem", chassisId);
        return;
    }
    getDumpServiceInfo(asyncResp, "System");
}

inline void handleLogServicesDumpEntriesCollectionGet(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }
    getDumpEntryCollection(asyncResp, dumpType);
}

inline void handleLogServicesDumpEntriesCollectionComputerSystemGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (chassisId != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem", chassisId);
        return;
    }
    getDumpEntryCollection(asyncResp, "System");
}

inline void handleLogServicesDumpEntryGet(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& dumpId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }
    getDumpEntryById(asyncResp, dumpId, dumpType);
}

inline void handleLogServicesDumpEntryComputerSystemGet(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::string& dumpId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (chassisId != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem", chassisId);
        return;
    }
    getDumpEntryById(asyncResp, dumpId, "System");
}

inline void handleLogServicesDumpEntryDelete(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& dumpId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }
    deleteDumpEntry(asyncResp, dumpId, dumpType);
}

inline void handleLogServicesDumpEntryComputerSystemDelete(
    crow::App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& chassisId, const std::string& dumpId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (chassisId != BMCWEB_REDFISH_SYSTEM_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "ComputerSystem", chassisId);
        return;
    }
    deleteDumpEntry(asyncResp, dumpId, "System");
}

inline void handleLogServicesDumpEntryDownloadGet(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& dumpId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }
    downloadDumpEntry(asyncResp, dumpId, dumpType);
}

inline void handleDBusEventLogEntryDownloadGet(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& systemName, const std::string& entryID)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (!http_helpers::isContentTypeAllowed(
            req.getHeaderValue("Accept"),
            http_helpers::ContentType::OctetStream, true))
    {
        asyncResp->res.result(boost::beast::http::status::bad_request);
        return;
    }
    downloadEventLogEntry(asyncResp, systemName, entryID, dumpType);
}

inline void handleLogServicesDumpCollectDiagnosticDataPost(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    createDump(asyncResp, req, dumpType);
}

inline void handleLogServicesDumpCollectDiagnosticDataComputerSystemPost(
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
    createDump(asyncResp, req, "System");
}

inline void handleLogServicesDumpClearLogPost(
    crow::App& app, const std::string& dumpType, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }
    clearDump(asyncResp, dumpType);
}

inline void handleLogServicesDumpClearLogComputerSystemPost(
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
    clearDump(asyncResp, "System");
}

inline void requestRoutesBMCDumpService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/Dump/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpServiceGet, std::ref(app), "BMC"));
}

inline void requestRoutesBMCDumpEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/Dump/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntriesCollectionGet, std::ref(app), "BMC"));
}

inline void requestRoutesBMCDumpEntry(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/<str>/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntryGet, std::ref(app), "BMC"));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/<str>/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(std::bind_front(
            handleLogServicesDumpEntryDelete, std::ref(app), "BMC"));
}

inline void requestRoutesBMCDumpEntryDownload(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/LogServices/Dump/Entries/<str>/attachment/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntryDownloadGet, std::ref(app), "BMC"));
}

inline void requestRoutesBMCDumpCreate(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/LogServices/Dump/Actions/LogService.CollectDiagnosticData/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleLogServicesDumpCollectDiagnosticDataPost,
                            std::ref(app), "BMC"));
}

inline void requestRoutesBMCDumpClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/LogServices/Dump/Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleLogServicesDumpClearLogPost, std::ref(app), "BMC"));
}

inline void requestRoutesDBusEventLogEntryDownload(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/EventLog/Entries/<str>/attachment/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleDBusEventLogEntryDownloadGet, std::ref(app), "System"));
}

inline void requestRoutesFaultLogDumpService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/LogServices/FaultLog/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpServiceGet, std::ref(app), "FaultLog"));
}

inline void requestRoutesFaultLogDumpEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/<str>/LogServices/FaultLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLogServicesDumpEntriesCollectionGet,
                            std::ref(app), "FaultLog"));
}

inline void requestRoutesFaultLogDumpEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/LogServices/FaultLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntryGet, std::ref(app), "FaultLog"));

    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/LogServices/FaultLog/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(std::bind_front(
            handleLogServicesDumpEntryDelete, std::ref(app), "FaultLog"));
}

inline void requestRoutesFaultLogDumpClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/LogServices/FaultLog/Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleLogServicesDumpClearLogPost, std::ref(app), "FaultLog"));
}

inline void requestRoutesSystemDumpService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Dump/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpServiceComputerSystemGet, std::ref(app)));
}

inline void requestRoutesSystemDumpEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Dump/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntriesCollectionComputerSystemGet,
            std::ref(app)));
}

inline void requestRoutesSystemDumpEntry(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleLogServicesDumpEntryComputerSystemGet, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(std::bind_front(
            handleLogServicesDumpEntryComputerSystemDelete, std::ref(app)));
}

inline void requestRoutesSystemDumpCreate(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/Dump/Actions/LogService.CollectDiagnosticData/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleLogServicesDumpCollectDiagnosticDataComputerSystemPost,
            std::ref(app)));
}

inline void requestRoutesSystemDumpClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/Dump/Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleLogServicesDumpClearLogComputerSystemPost, std::ref(app)));
}

inline void requestRoutesCrashdumpService(App& app)
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
            std::format("/redfish/v1/Systems/{}/LogServices/Crashdump",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["Name"] = "Open BMC Oem Crashdump Service";
        asyncResp->res.jsonValue["Description"] = "Oem Crashdump Service";
        asyncResp->res.jsonValue["Id"] = "Crashdump";
        asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";
        asyncResp->res.jsonValue["MaxNumberOfRecords"] = 10;

        std::pair<std::string, std::string> redfishDateTimeOffset =
            redfish::time_utils::getDateTimeOffsetNow();
        asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
        asyncResp->res.jsonValue["DateTimeLocalOffset"] =
            redfishDateTimeOffset.second;

        asyncResp->res.jsonValue["Entries"]["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/Crashdump/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/LogService.ClearLog",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.CollectDiagnosticData"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/LogService.CollectDiagnosticData",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#Oem/Crashdump.Configuration"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/Crashdump/Actions/Oem/Crashdump.Configuration",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
    });
}

void inline requestRoutesCrashdumpClear(App& app)
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
        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code& ec,
                        const std::string&) {
            if (ec)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
        },
            crashdumpObject, crashdumpPath, deleteAllInterface, "DeleteAll");
    });
}

static void
    logCrashdumpEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& logID, nlohmann::json& logEntryJson)
{
    auto getStoredLogCallback =
        [asyncResp, logID,
         &logEntryJson](const boost::system::error_code& ec,
                        const dbus::utility::DBusPropertiesMap& params) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("failed to get log ec: {}", ec.message());
            if (ec.value() ==
                boost::system::linux_error::bad_request_descriptor)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
            }
            else
            {
                messages::internalError(asyncResp->res);
            }
            return;
        }

        std::string timestamp{};
        std::string filename{};
        std::string logfile{};
        parseCrashdumpParameters(params, filename, timestamp, logfile);

        if (filename.empty() || timestamp.empty())
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
            return;
        }

        std::string crashdumpURI =
            std::format("/redfish/v1/Systems/{}/LogServices/Crashdump/Entries/",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME) +
            logID + "/" + filename;
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
            DiagnosticDataTypeString = "DramCecc_RuntimeError_APMLCrashdump";
        else if (filename.find("pcie-runtime") != std::string::npos)
            DiagnosticDataTypeString = "Pcie_RuntimeError_APMLCrashdump";
        else
            DiagnosticDataTypeString = "PECICrashdump";

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
        *crow::connections::systemBus, crashdumpObject,
        crashdumpPath + std::string("/") + logID, crashdumpInterface,
        std::move(getStoredLogCallback));
}

inline void requestRoutesCrashdumpEntryCollection(App& app)
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

        constexpr std::array<std::string_view, 1> interfaces = {
            crashdumpInterface};
        dbus::utility::getSubTreePaths(
            "/", 0, interfaces,
            [asyncResp](const boost::system::error_code& ec,
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
            asyncResp->res.jsonValue["Name"] = "Open BMC Crashdump Entries";
            asyncResp->res.jsonValue["Description"] =
                "Collection of Crashdump Entries";
            asyncResp->res.jsonValue["Members"] = nlohmann::json::array();
            asyncResp->res.jsonValue["Members@odata.count"] = 0;

            for (const std::string& path : resp)
            {
                const sdbusplus::message::object_path objPath(path);
                // Get the log ID
                std::string logID = objPath.filename();
                if (logID.empty())
                {
                    continue;
                }
                // Add the log entry to the array
                logCrashdumpEntry(asyncResp, logID,
                                  asyncResp->res.jsonValue["Members"]);
            }
        });
    });
}

inline void requestRoutesCrashdumpEntry(App& app)
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
        const std::string& logID = param;
        logCrashdumpEntry(asyncResp, logID, asyncResp->res.jsonValue);
    });
}

inline void requestRoutesCrashdumpFile(App& app)
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
        // Do not call getRedfishRoute here since the crashdump file is not a
        // Redfish resource.

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

        auto getStoredLogCallback =
            [asyncResp, logID, fileName, url(boost::urls::url(req.url()))](
                const boost::system::error_code& ec,
                const std::vector<
                    std::pair<std::string, dbus::utility::DbusVariantType>>&
                    resp) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG("failed to get log ec: {}", ec.message());
                messages::internalError(asyncResp->res);
                return;
            }

            std::string dbusFilename{};
            std::string dbusTimestamp{};
            std::string dbusFilepath{};

            parseCrashdumpParameters(resp, dbusFilename, dbusTimestamp,
                                     dbusFilepath);

            if (dbusFilename.empty() || dbusTimestamp.empty() ||
                dbusFilepath.empty())
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            // Verify the file name parameter is correct
            if (fileName != dbusFilename)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            if (!asyncResp->res.openFile(dbusFilepath))
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", logID);
                return;
            }

            // Configure this to be a file download when accessed
            // from a browser
            asyncResp->res.addHeader(
                boost::beast::http::field::content_disposition, "attachment");
        };
        sdbusplus::asio::getAllProperties(
            *crow::connections::systemBus, crashdumpObject,
            crashdumpPath + std::string("/") + logID, crashdumpInterface,
            std::move(getStoredLogCallback));
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
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
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

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/Crashdump/"
                        "Actions/Oem/Crashdump.Configuration",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);

        sdbusplus::asio::getProperty<ConfigTable>(
            *crow::connections::systemBus, "com.amd.RAS", "/com/amd/RAS",
            "com.amd.RAS.Configuration", "RasConfigTable",
            [asyncResp](const boost::system::error_code& ec,
                        const ConfigTable& rasConfigTable) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG("DBUS RAS Config response error {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            nlohmann::json jsonConfigTable = nlohmann::json::object();

            for (const auto& [key, tuple] : rasConfigTable)
            {
                const auto& variant = std::get<2>(
                    tuple); // Extract the variant (third element of the tuple)

                std::visit([&jsonConfigTable, &key](auto&& value) {
                    using T =
                        std::decay_t<decltype(value)>; // Get the actual type of
                                                       // the value
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        jsonConfigTable[key] = value; // Handle bool type
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        jsonConfigTable[key] = value; // Handle std::string type
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        jsonConfigTable[key] = value; // Handle int64_t type
                    }
                    else if constexpr (std::is_same_v<T,
                                                      std::vector<std::string>>)
                    {
                        jsonConfigTable[key] =
                            value; // Handle vector<std::string> type
                    }
                    else if constexpr (std::is_same_v<T, std::map<std::string,
                                                                  std::string>>)
                    {
                        jsonConfigTable[key] =
                            value; // Handle map<string, string> type
                    }
                    else
                    {
                        BMCWEB_LOG_DEBUG("Unknown variant type encountered.");
                    }
                }, variant);
            }
            asyncResp->res.jsonValue["ConfigTable"] = jsonConfigTable;
        });
    });

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/Crashdump/"
                      "Actions/Oem/Crashdump.Configuration")
        .privileges({{"ConfigureComponents"}})
        .methods(boost::beast::http::verb::patch)(
            [&app](const crow::Request& req,
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
        std::optional<std::map<std::string, std::string>> aifsSignatureIdList;
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
        std::optional<int64_t> PcieAerErrThresholdCnt;

        if (!redfish::json_util::readJsonAction(
                req, asyncResp->res, "AifsSignatureIdList", aifsSignatureIdList,
                "ApmlRetries", apmlRetries, "SystemRecoveryMode",
                SystemRecoveryMode, "ResetSignalType", ResetSignalType,
                "HarvestMicrocode", HarvestMicrocode, "HarvestPPIN",
                HarvestPPIN, "SigIdOffset", SigIdOffset, "AifsArmed", aifsArmed,
                "DisableAifsResetOnSyncfloodCounter",
                DisableAifsResetOnSyncfloodCounter, "DramCeccPollingEn",
                DramCeccPollingEn, "McaPollingEn", McaPollingEn,
                "PcieAerPollingEn", PcieAerPollingEn, "DramCeccThresholdEn",
                DramCeccThresholdEn, "McaThresholdEn", McaThresholdEn,
                "PcieAerThresholdEn", PcieAerThresholdEn, "McaPollingPeriod",
                McaPollingPeriod, "DramCeccPollingPeriod",
                DramCeccPollingPeriod, "PcieAerPollingPeriod",
                PcieAerPollingPeriod, "DramCeccErrThresholdCnt",
                DramCeccErrThresholdCnt, "McaErrThresholdCnt",
                McaErrThresholdCnt, "PcieAerErrThresholdCnt",
                PcieAerErrThresholdCnt))
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
                "SetAttribute", "AifsArmed", std::variant<bool>(*aifsArmed));
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
                "SetAttribute", "McaErrThresholdCnt",
                std::variant<int64_t>(*McaErrThresholdCnt));
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
            }, "com.amd.RAS", "/com/amd/RAS", "com.amd.RAS.Configuration",
                "SetAttribute", "PcieAerErrThresholdCnt",
                std::variant<int64_t>(*PcieAerErrThresholdCnt));
        }
    });
}

enum class OEMDiagnosticType
{
    onDemand,
    telemetry,
    invalid,
};

inline OEMDiagnosticType getOEMDiagnosticType(std::string_view oemDiagStr)
{
    if (oemDiagStr == "OnDemand")
    {
        return OEMDiagnosticType::onDemand;
    }
    if (oemDiagStr == "Telemetry")
    {
        return OEMDiagnosticType::telemetry;
    }

    return OEMDiagnosticType::invalid;
}

inline void requestRoutesCrashdumpCollect(App& app)
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
        if (!redfish::json_util::readJsonAction(
                req, asyncResp->res, "DiagnosticDataType", diagnosticDataType,
                "OEMDiagnosticDataType", oemDiagnosticDataType))
        {
            return;
        }

        if (diagnosticDataType != "OEM")
        {
            BMCWEB_LOG_ERROR(
                "Only OEM DiagnosticDataType supported for Crashdump");
            messages::actionParameterValueFormatError(
                asyncResp->res, diagnosticDataType, "DiagnosticDataType",
                "CollectDiagnosticData");
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
            taskMatchStr = "type='signal',"
                           "interface='org.freedesktop.DBus.Properties',"
                           "member='PropertiesChanged',"
                           "arg0namespace='com.intel.crashdump'";
        }
        else if (oemDiagType == OEMDiagnosticType::telemetry)
        {
            iface = crashdumpTelemetryInterface;
            method = "GenerateTelemetryLog";
            taskMatchStr = "type='signal',"
                           "interface='org.freedesktop.DBus.Properties',"
                           "member='PropertiesChanged',"
                           "arg0namespace='com.intel.crashdump'";
        }
        else
        {
            BMCWEB_LOG_ERROR("Unsupported OEMDiagnosticDataType: {}",
                             oemDiagnosticDataType);
            messages::actionParameterValueFormatError(
                asyncResp->res, oemDiagnosticDataType, "OEMDiagnosticDataType",
                "CollectDiagnosticData");
            return;
        }

        auto collectCrashdumpCallback =
            [asyncResp, payload(task::Payload(req)),
             taskMatchStr](const boost::system::error_code& ec,
                           const std::string&) mutable {
            if (ec)
            {
                if (ec.value() == boost::system::errc::operation_not_supported)
                {
                    messages::resourceInStandby(asyncResp->res);
                }
                else if (ec.value() ==
                         boost::system::errc::device_or_resource_busy)
                {
                    messages::serviceTemporarilyUnavailable(asyncResp->res,
                                                            "60");
                }
                else
                {
                    messages::internalError(asyncResp->res);
                }
                return;
            }
            std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
                [](const boost::system::error_code& ec2, sdbusplus::message_t&,
                   const std::shared_ptr<task::TaskData>& taskData) {
                if (!ec2)
                {
                    taskData->messages.emplace_back(messages::taskCompletedOK(
                        std::to_string(taskData->index)));
                    taskData->state = "Completed";
                }
                return task::completed;
            },
                taskMatchStr);

            task->startTimer(std::chrono::minutes(5));
            task->populateResp(asyncResp->res);
            task->payload.emplace(std::move(payload));
        };

        crow::connections::systemBus->async_method_call(
            std::move(collectCrashdumpCallback), crashdumpObject, crashdumpPath,
            iface, method);
    });
}

// PPR

inline void requestRoutesPprService(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/")
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

        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/PostPackageRepair",
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

        asyncResp->res.jsonValue["Actions"]["#LogService.pprStatus"]["target"] =
            std::format(
                "/redfish/v1/Systems/{}/LogServices/PostPackageRepair/Status",
                BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Actions"]["#LogService.pprConfig"]["target"] =
            std::format(
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
bool oobPprEnable = false;

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
            BMCWEB_LOG_ERROR("DBUS POST Package Repair Record Add error: {} ",
                             ec1);
            messages::internalError(asyncResp->res);
            return;
        }
        BMCWEB_LOG_ERROR("DBUS POST Package Repair Record Add Start {} ",
                         int(recordAdd));

        crow::connections::systemBus->async_method_call(
            [asyncResp, RecordAdd, Index](const boost::system::error_code ec2) {
            if (ec2)
            {
                BMCWEB_LOG_ERROR("D-Bus responses error: {} ", ec2);
                messages::internalError(asyncResp->res);
                return;
            }
            BMCWEB_LOG_ERROR("DBUS POST Package Repair Record Add success ");
            crow::connections::systemBus->async_method_call(
                [asyncResp, Index](const boost::system::error_code ec3,
                                   const uint32_t& startRuntimeRepair) {
                if (ec3)
                {
                    BMCWEB_LOG_ERROR("DBUS start Runtime Repair error: {} ",
                                     ec3);
                    messages::internalError(asyncResp->res);
                    return;
                }
                BMCWEB_LOG_ERROR("DBUS success start Runtime Repair : Start {}",
                                 startRuntimeRepair);
            },
                pprFileObject, pprFilePath, pprFileInterface,
                "startRuntimeRepair", Index);
        }, pprFileObject, pprFilePath, "org.freedesktop.DBus.Properties", "Set",
            pprFileInterface, "RecordAdd", std::variant<bool>(*RecordAdd));
    },
        pprFileObject, pprFilePath, pprFileInterface,
        "setPostPackageRepairData", repairEntryNum, repairType, socNum,
        payload);
}

void inline requestRoutesPprFile(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/PostPackageRepair/RepairData")
        .privileges(redfish::privileges::patchLogEntry)
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

            if (!json_util::readJson(el.value(), asyncResp->res, "RepairType",
                                     RepairType, "RepairEntryNum",
                                     RepairEntryNum, "SocNum", SocNum,
                                     "Payload", Payload))
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
    });
}

// PPR Status

void inline requestRoutesPprStatus(App& app)
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
                const std::vector<std::tuple<uint16_t, uint16_t, uint16_t,
                                             uint16_t, std::vector<uint16_t>>>&
                    postpackagerepairstatus) {
            BMCWEB_LOG_ERROR("requestRoutesPprStatus start {}", ec);
            if (ec)
            {
                BMCWEB_LOG_ERROR("requestRoutesPprStatus got error {}", ec);
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
                std::vector<uint16_t> payload = std::get<4>(resolveList);

                nlohmann::json jsonPpr = {{"repairEntryNum", repairEntryNum},
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
            pprFileObject, pprFilePath, pprFileInterface,
            "getPostPackageRepairStatus");
    });
}

// PPR Config

void inline requestRoutesPprGetConfig(App& app)
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
            [asyncResp](const boost::system::error_code& ec,
                        std::vector<uint16_t>& postpackagerepairconfig) {
            BMCWEB_LOG_ERROR("requestRoutesGetPprConfig start {}", ec);
            if (ec)
            {
                BMCWEB_LOG_ERROR("requestRoutesGetPprConfig got error {}", ec);
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

            nlohmann::json jsonPpr = {{"OobPprEnable", oobPprEnable},
                                      {"autoScheduleRtAsBtPpr", RtToBt},
                                      {"autoScheduleBtAsHard", BtSetToHard}};
            pprConfig.push_back(jsonPpr);

            asyncResp->res.jsonValue["Members"] = pprConfig;
            asyncResp->res.jsonValue["Members@odata.count"] = 1;

            messages::success(asyncResp->res);
        },
            pprFileObject, pprFilePath, pprFileInterface,
            "getPostPackageRepairConfig");
    });
}

void inline requestRoutesPprSetConfig(App& app)
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
                req, asyncResp->res, "autoScheduleBtAsHard", BtSetToHard,
                "autoScheduleRtAsBtPpr", RtToBt))
        {
            BMCWEB_LOG_ERROR("requestRoutesPprSetConfig readJson Error ");
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
            BMCWEB_LOG_ERROR("requestRoutesPprSetConfig readJson Flag is 0 ");
            return;
        }

        crow::connections::systemBus->async_method_call(
            [asyncResp, flag, data](const boost::system::error_code& ec,
                                    bool& result) {
            BMCWEB_LOG_ERROR("requestRoutesPprSetConfig start {}", ec);
            if (ec)
            {
                BMCWEB_LOG_ERROR("requestRoutesPprSetConfig got error {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            BMCWEB_LOG_ERROR("requestRoutesPprSetConfig end Result {}",
                             int(result));
            messages::success(asyncResp->res);
        },
            pprFileObject, pprFilePath, pprFileInterface,
            "setPostPackageRepairConfig", flag, data);
    });
}

/**
 * DBusLogServiceActionsClear class supports POST method for ClearLog action.
 */
inline void requestRoutesDBusLogServiceActionsClear(App& app)
{
    /**
     * Function handles POST method request.
     * The Clear Log actions does not require any parameter.The action deletes
     * all entries found in the Entries collection for this Log Service.
     */

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/EventLog/Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
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
        BMCWEB_LOG_DEBUG("Do delete all entries.");

        // Process response from Logging service.
        auto respHandler = [asyncResp](const boost::system::error_code& ec) {
            BMCWEB_LOG_DEBUG("doClearLog resp_handler callback: Done");
            if (ec)
            {
                // TODO Handle for specific error code
                BMCWEB_LOG_ERROR("doClearLog resp_handler got error {}", ec);
                asyncResp->res.result(
                    boost::beast::http::status::internal_server_error);
                return;
            }

            asyncResp->res.result(boost::beast::http::status::no_content);
        };

        // Make call to Logging service to request Clear Log
        crow::connections::systemBus->async_method_call(
            respHandler, "xyz.openbmc_project.Logging",
            "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
    });
}

uint8_t getUrlHostNumber(const crow::Request& req)
{
    uint8_t hostNumber = 0;
    boost::urls::url_view urlView = req.url();

    for (const auto& param : urlView.params())
    {
        if (param.key == "HostNumber" && !param.value.empty())
        {
            try
            {
                int temp = std::stoi(std::string(param.value));
                hostNumber = static_cast<uint8_t>(temp);
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
    return hostNumber;
}

/****************************************************
 * Redfish PostCode interfaces
 * using DBUS interface: getPostCodesTS
 ******************************************************/
inline void requestRoutesPostCodesLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/<str>/LogServices/PostCodes/")
        .privileges(redfish::privileges::getLogService)
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
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/PostCodes",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["@odata.type"] =
            "#LogService.v1_2_0.LogService";
        asyncResp->res.jsonValue["Name"] = "POST Code Log Service";
        asyncResp->res.jsonValue["Description"] = "POST Code Log Service";
        asyncResp->res.jsonValue["Id"] = "PostCodes";
        asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";
        asyncResp->res.jsonValue["Entries"]["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/PostCodes/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);

        std::pair<std::string, std::string> redfishDateTimeOffset =
            redfish::time_utils::getDateTimeOffsetNow();
        asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
        asyncResp->res.jsonValue["DateTimeLocalOffset"] =
            redfishDateTimeOffset.second;

        asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"]
                                ["target"] = std::format(
            "/redfish/v1/Systems/{}/LogServices/PostCodes/Actions/LogService.ClearLog",
            BMCWEB_REDFISH_SYSTEM_URI_NAME);
    });
}

inline void requestRoutesPostCodesClear(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/PostCodes/Actions/LogService.ClearLog/")
        // The following privilege is incorrect;  It should be ConfigureManager
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
        if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }
        BMCWEB_LOG_DEBUG("Do delete all postcodes entries.");

        uint8_t hostNumber = getUrlHostNumber(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
        }

        std::string service = "xyz.openbmc_project.State.Boot.PostCode" +
                              std::to_string(hostNumber);

        std::string objectPath = "/xyz/openbmc_project/State/Boot/PostCode" +
                                 std::to_string(hostNumber);

        // Make call to post-code service to request clear all
        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code& ec) {
            if (ec)
            {
                // TODO Handle for specific error code
                BMCWEB_LOG_ERROR("doClearPostCodes resp_handler got error {}",
                                 ec);
                asyncResp->res.result(
                    boost::beast::http::status::internal_server_error);
                messages::internalError(asyncResp->res);
                return;
            }
            messages::success(asyncResp->res);
        }, service, objectPath, "xyz.openbmc_project.Collection.DeleteAll",
            "DeleteAll");
    });
}

/**
 * @brief Parse post code ID and get the current value and index value
 *        eg: postCodeID=B1-2, currentValue=1, index=2
 *
 * @param[in]  postCodeID     Post Code ID
 * @param[out] currentValue   Current value
 * @param[out] index          Index value
 *
 * @return bool true if the parsing is successful, false the parsing fails
 */
inline bool parsePostCode(std::string_view postCodeID, uint64_t& currentValue,
                          uint16_t& index)
{
    std::vector<std::string> split;
    bmcweb::split(split, postCodeID, '-');
    if (split.size() != 2)
    {
        return false;
    }
    std::string_view postCodeNumber = split[0];
    if (postCodeNumber.size() < 2)
    {
        return false;
    }
    if (postCodeNumber[0] != 'B')
    {
        return false;
    }
    postCodeNumber.remove_prefix(1);
    auto [ptrIndex, ecIndex] = std::from_chars(postCodeNumber.begin(),
                                               postCodeNumber.end(), index);
    if (ptrIndex != postCodeNumber.end() || ecIndex != std::errc())
    {
        return false;
    }

    std::string_view postCodeIndex = split[1];

    auto [ptrValue, ecValue] = std::from_chars(
        postCodeIndex.begin(), postCodeIndex.end(), currentValue);

    return ptrValue == postCodeIndex.end() && ecValue == std::errc();
}

static bool fillPostCodeEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const boost::container::flat_map<
        uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>& postcode,
    const uint16_t bootIndex, const uint64_t codeIndex = 0,
    const uint64_t skip = 0, const uint64_t top = 0)
{
    // Get the Message from the MessageRegistry
    const registries::Message* message =
        registries::getMessage("OpenBMC.0.2.BIOSPOSTCode");
    if (message == nullptr)
    {
        BMCWEB_LOG_ERROR("Couldn't find known message?");
        return false;
    }
    uint64_t currentCodeIndex = 0;
    uint64_t firstCodeTimeUs = 0;
    for (const std::pair<uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
             code : postcode)
    {
        currentCodeIndex++;
        std::string postcodeEntryID =
            "B" + std::to_string(bootIndex) + "-" +
            std::to_string(currentCodeIndex); // 1 based index in EntryID string

        uint64_t usecSinceEpoch = code.first;
        uint64_t usTimeOffset = 0;

        if (1 == currentCodeIndex)
        { // already incremented
            firstCodeTimeUs = code.first;
        }
        else
        {
            usTimeOffset = code.first - firstCodeTimeUs;
        }

        // skip if no specific codeIndex is specified and currentCodeIndex does
        // not fall between top and skip
        if ((codeIndex == 0) &&
            (currentCodeIndex <= skip || currentCodeIndex > top))
        {
            continue;
        }

        // skip if a specific codeIndex is specified and does not match the
        // currentIndex
        if ((codeIndex > 0) && (currentCodeIndex != codeIndex))
        {
            // This is done for simplicity. 1st entry is needed to calculate
            // time offset. To improve efficiency, one can get to the entry
            // directly (possibly with flatmap's nth method)
            continue;
        }

        // currentCodeIndex is within top and skip or equal to specified code
        // index

        // Get the Created time from the timestamp
        std::string entryTimeStr;
        entryTimeStr = redfish::time_utils::getDateTimeUintUs(usecSinceEpoch);

        // assemble messageArgs: BootIndex, TimeOffset(100us), PostCode(hex)
        std::ostringstream hexCode;
        hexCode << "0x" << std::setfill('0') << std::setw(2) << std::hex
                << std::get<0>(code.second);
        std::ostringstream timeOffsetStr;
        // Set Fixed -Point Notation
        timeOffsetStr << std::fixed;
        // Set precision to 4 digits
        timeOffsetStr << std::setprecision(4);
        // Add double to stream
        timeOffsetStr << static_cast<double>(usTimeOffset) / 1000 / 1000;

        std::string bootIndexStr = std::to_string(bootIndex);
        std::string timeOffsetString = timeOffsetStr.str();
        std::string hexCodeStr = hexCode.str();

        std::array<std::string_view, 3> messageArgs = {
            bootIndexStr, timeOffsetString, hexCodeStr};

        std::string msg =
            redfish::registries::fillMessageArgs(messageArgs, message->message);
        if (msg.empty())
        {
            messages::internalError(asyncResp->res);
            return false;
        }

        // Get Severity template from message registry
        std::string severity;
        if (message != nullptr)
        {
            severity = message->messageSeverity;
        }

        // Format entry
        nlohmann::json::object_t bmcLogEntry;
        bmcLogEntry["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
        bmcLogEntry["@odata.id"] = boost::urls::format(
            "/redfish/v1/Systems/{}/LogServices/PostCodes/Entries/{}",
            BMCWEB_REDFISH_SYSTEM_URI_NAME, postcodeEntryID);
        bmcLogEntry["Name"] = "POST Code Log Entry";
        bmcLogEntry["Id"] = postcodeEntryID;
        bmcLogEntry["Message"] = std::move(msg);
        bmcLogEntry["MessageId"] = "OpenBMC.0.2.BIOSPOSTCode";
        bmcLogEntry["MessageArgs"] = messageArgs;
        bmcLogEntry["EntryType"] = "Event";
        bmcLogEntry["Severity"] = std::move(severity);
        bmcLogEntry["Created"] = entryTimeStr;
        if (!std::get<std::vector<uint8_t>>(code.second).empty())
        {
            bmcLogEntry["AdditionalDataURI"] =
                std::format(
                    "/redfish/v1/Systems/{}/LogServices/PostCodes/Entries/",
                    BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                postcodeEntryID + "/attachment";
        }

        // codeIndex is only specified when querying single entry, return only
        // that entry in this case
        if (codeIndex != 0)
        {
            asyncResp->res.jsonValue.update(bmcLogEntry);
            return true;
        }

        nlohmann::json& logEntryArray = asyncResp->res.jsonValue["Members"];
        logEntryArray.emplace_back(std::move(bmcLogEntry));
    }

    // Return value is always false when querying multiple entries
    return false;
}

static void
    getPostCodeForEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::string& entryId, uint8_t hostNumber)
{
    uint16_t bootIndex = 0;
    uint64_t codeIndex = 0;
    if (!parsePostCode(entryId, codeIndex, bootIndex))
    {
        // Requested ID was not found
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryId);
        return;
    }

    if (bootIndex == 0 || codeIndex == 0)
    {
        // 0 is an invalid index
        messages::resourceNotFound(asyncResp->res, "LogEntry", entryId);
        return;
    }

    std::string service = "xyz.openbmc_project.State.Boot.PostCode" +
                          std::to_string(hostNumber);

    std::string objectPath = "/xyz/openbmc_project/State/Boot/PostCode" +
                             std::to_string(hostNumber);

    crow::connections::systemBus->async_method_call(
        [asyncResp, entryId, bootIndex,
         codeIndex](const boost::system::error_code& ec,
                    const boost::container::flat_map<
                        uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
                        postcode) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("DBUS POST CODE PostCode response error");
            messages::internalError(asyncResp->res);
            return;
        }

        if (postcode.empty())
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", entryId);
            return;
        }

        if (!fillPostCodeEntry(asyncResp, postcode, bootIndex, codeIndex))
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", entryId);
            return;
        }
    },
        service, objectPath, "xyz.openbmc_project.State.Boot.PostCode",
        "GetPostCodesWithTimeStamp", bootIndex);
}

static void
    getPostCodeForBoot(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const uint16_t bootIndex, const uint16_t bootCount,
                       const uint64_t entryCount, size_t skip, size_t top,
                       uint8_t hostNumber)
{
    std::string service = "xyz.openbmc_project.State.Boot.PostCode" +
                          std::to_string(hostNumber);

    std::string objectPath = "/xyz/openbmc_project/State/Boot/PostCode" +
                             std::to_string(hostNumber);

    crow::connections::systemBus->async_method_call(
        [asyncResp, bootIndex, bootCount, entryCount, skip, top,
         hostNumber](const boost::system::error_code& ec,
                     const boost::container::flat_map<
                         uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
                         postcode) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("DBUS POST CODE PostCode response error");
            messages::internalError(asyncResp->res);
            return;
        }

        uint64_t endCount = entryCount;
        if (!postcode.empty())
        {
            endCount = entryCount + postcode.size();
            if (skip < endCount && (top + skip) > entryCount)
            {
                uint64_t thisBootSkip = std::max(static_cast<uint64_t>(skip),
                                                 entryCount) -
                                        entryCount;
                uint64_t thisBootTop =
                    std::min(static_cast<uint64_t>(top + skip), endCount) -
                    entryCount;

                fillPostCodeEntry(asyncResp, postcode, bootIndex, 0,
                                  thisBootSkip, thisBootTop);
            }
            asyncResp->res.jsonValue["Members@odata.count"] = endCount;
        }

        // continue to previous bootIndex
        if (bootIndex < bootCount)
        {
            getPostCodeForBoot(asyncResp, static_cast<uint16_t>(bootIndex + 1),
                               bootCount, endCount, skip, top, hostNumber);
        }
        else if (skip + top < endCount)
        {
            asyncResp->res.jsonValue["Members@odata.nextLink"] =
                std::format(
                    "/redfish/v1/Systems/{}/LogServices/PostCodes/Entries?$skip=",
                    BMCWEB_REDFISH_SYSTEM_URI_NAME) +
                std::to_string(skip + top);
        }
    },
        service, objectPath, "xyz.openbmc_project.State.Boot.PostCode",
        "GetPostCodesWithTimeStamp", bootIndex);
}

static void
    getCurrentBootNumber(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         size_t skip, size_t top, uint8_t hostNumber)
{
    uint64_t entryCount = 0;
    std::string service = "xyz.openbmc_project.State.Boot.PostCode" +
                          std::to_string(hostNumber);

    std::string objectPath = "/xyz/openbmc_project/State/Boot/PostCode" +
                             std::to_string(hostNumber);

    sdbusplus::asio::getProperty<uint16_t>(
        *crow::connections::systemBus, service, objectPath,
        "xyz.openbmc_project.State.Boot.PostCode", "CurrentBootCycleCount",
        [asyncResp, entryCount, skip, top, hostNumber](
            const boost::system::error_code& ec, const uint16_t bootCount) {
        if (ec)
        {
            BMCWEB_LOG_DEBUG("DBUS response error {}", ec);
            messages::internalError(asyncResp->res);
            return;
        }
        getPostCodeForBoot(asyncResp, 1, bootCount, entryCount, skip, top,
                           hostNumber);
    });
}

inline void requestRoutesPostCodesEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/<str>/LogServices/PostCodes/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName) {
        query_param::QueryCapabilities capabilities = {
            .canDelegateTop = true,
            .canDelegateSkip = true,
        };
        query_param::Query delegatedQuery;
        if (!redfish::setUpRedfishRouteWithDelegation(
                app, req, asyncResp, delegatedQuery, capabilities))
        {
            return;
        }

        if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        uint8_t hostNumber = getUrlHostNumber(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
        }

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            std::format("/redfish/v1/Systems/{}/LogServices/PostCodes/Entries",
                        BMCWEB_REDFISH_SYSTEM_URI_NAME);
        asyncResp->res.jsonValue["Name"] = "BIOS POST Code Log Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of POST Code Log Entries";
        asyncResp->res.jsonValue["Members"] = nlohmann::json::array();
        asyncResp->res.jsonValue["Members@odata.count"] = 0;
        size_t skip = delegatedQuery.skip.value_or(0);
        size_t top = delegatedQuery.top.value_or(query_param::Query::maxTop);
        getCurrentBootNumber(asyncResp, skip, top, hostNumber);
    });
}

inline void requestRoutesPostCodesEntryAdditionalData(App& app)
{
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/<str>/LogServices/PostCodes/Entries/<str>/attachment/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName,
                   const std::string& postCodeID) {
        if (!redfish::setUpRedfishRoute(app, req, asyncResp))
        {
            return;
        }
        if (!http_helpers::isContentTypeAllowed(
                req.getHeaderValue("Accept"),
                http_helpers::ContentType::OctetStream, true))
        {
            asyncResp->res.result(boost::beast::http::status::bad_request);
            return;
        }
        if (systemName != BMCWEB_REDFISH_SYSTEM_URI_NAME)
        {
            messages::resourceNotFound(asyncResp->res, "ComputerSystem",
                                       systemName);
            return;
        }

        uint64_t currentValue = 0;
        uint16_t index = 0;
        if (!parsePostCode(postCodeID, currentValue, index))
        {
            messages::resourceNotFound(asyncResp->res, "LogEntry", postCodeID);
            return;
        }

        uint8_t hostNumber = getUrlHostNumber(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
        }

        std::string service = "xyz.openbmc_project.State.Boot.PostCode" +
                              std::to_string(hostNumber);

        std::string objectPath = "/xyz/openbmc_project/State/Boot/PostCode" +
                                 std::to_string(hostNumber);

        crow::connections::systemBus->async_method_call(
            [asyncResp, postCodeID, currentValue](
                const boost::system::error_code& ec,
                const std::vector<std::tuple<uint64_t, std::vector<uint8_t>>>&
                    postcodes) {
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry",
                                           postCodeID);
                return;
            }
            if (ec)
            {
                BMCWEB_LOG_DEBUG("DBUS response error {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            size_t value = static_cast<size_t>(currentValue) - 1;
            if (value == std::string::npos || postcodes.size() < currentValue)
            {
                BMCWEB_LOG_WARNING("Wrong currentValue value");
                messages::resourceNotFound(asyncResp->res, "LogEntry",
                                           postCodeID);
                return;
            }

            const auto& [tID, c] = postcodes[value];
            if (c.empty())
            {
                BMCWEB_LOG_WARNING("No found post code data");
                messages::resourceNotFound(asyncResp->res, "LogEntry",
                                           postCodeID);
                return;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            const char* d = reinterpret_cast<const char*>(c.data());
            std::string_view strData(d, c.size());

            asyncResp->res.addHeader(boost::beast::http::field::content_type,
                                     "application/octet-stream");
            asyncResp->res.addHeader(
                boost::beast::http::field::content_transfer_encoding, "Base64");
            asyncResp->res.write(crow::utility::base64encode(strData));
        },
            service, objectPath, "xyz.openbmc_project.State.Boot.PostCode",
            "GetPostCodes", index);
    });
}

inline void requestRoutesPostCodesEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/<str>/LogServices/PostCodes/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [&app](const crow::Request& req,
                   const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& systemName, const std::string& targetID) {
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

        uint8_t hostNumber = getUrlHostNumber(req);
        if (hostNumber > 2)
        {
            messages::actionParameterNotSupported(
                asyncResp->res, std::to_string(hostNumber), "HostNumber");
        }

        getPostCodeForEntry(asyncResp, targetID, hostNumber);
    });
}

} // namespace redfish
