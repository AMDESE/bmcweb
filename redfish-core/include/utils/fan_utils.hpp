// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "async_resp.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "human_sort.hpp"
#include "logging.hpp"

#include <asm-generic/errno.h>

#include <boost/system/error_code.hpp>
#include <sdbusplus/message/native_types.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redfish
{
namespace fan_utils
{
constexpr std::array<std::string_view, 1> sensorInterface = {
    "xyz.openbmc_project.Sensor.Value"};

inline void afterGetFanSensorObjects(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::function<void(
        const std::vector<std::pair<std::string, std::string>>&)>& callback,
    const boost::system::error_code& ec,
    const dbus::utility::MapperGetSubTreeResponse& subtree)
{
    if (ec)
    {
        if (ec.value() != boost::system::errc::io_error && ec.value() != EBADR)
        {
            BMCWEB_LOG_ERROR("DBUS response error {}", ec);
            messages::internalError(asyncResp->res);
        }
        return;
    }

    std::vector<std::pair<std::string, std::string>> sensorsPathAndService;

    for (const auto& [sensorPath, serviceMaps] : subtree)
    {
        for (const auto& [service, interfaces] : serviceMaps)
        {
            sensorsPathAndService.emplace_back(service, sensorPath);
        }
    }

    /* Sort list by sensorPath */
    std::ranges::sort(
        sensorsPathAndService, [](const auto& c1, const auto& c2) {
            return AlphanumLess<std::string>()(c1.second, c2.second);
        });

    callback(sensorsPathAndService);
}

inline void getFanSensorObjects(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::object_path& fanPath,
    const std::function<void(
        const std::vector<std::pair<std::string, std::string>>&)>& callback)
{
    sdbusplus::object_path endpointPath{fanPath};
    endpointPath /= "sensors";

    dbus::utility::getAssociatedSubTree(
        endpointPath, sdbusplus::object_path("/xyz/openbmc_project/sensors"), 0,
        sensorInterface,
        std::bind_front(afterGetFanSensorObjects, asyncResp, callback));
}

inline void getFanPaths(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& validChassisPath,
    const std::function<void(const dbus::utility::MapperGetSubTreePathsResponse&
                                 fanPaths)>& callback)
{
    // Entity Manager on AMD platforms does not expose cooled_by /
    // Inventory.Item.Fan associations. Enumerate FanSensor objects instead.
    dbus::utility::getSubTreePaths(
        "/xyz/openbmc_project/sensors/fan_tach", 0, sensorInterface,
        [asyncResp, validChassisPath, callback](
            const boost::system::error_code& ec,
            const dbus::utility::MapperGetSubTreePathsResponse& subtreePaths) {
            if (ec)
            {
                if (ec.value() != boost::system::errc::io_error &&
                    ec.value() != EBADR)
                {
                    BMCWEB_LOG_ERROR("DBUS response error {}", ec);
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            // Type matching a(sss) -> tuple<Forward, Reverse, Endpoint>
            using AssociationDefinition = std::tuple<std::string, std::string, std::string>;
            auto filteredSubtreePaths = std::make_shared<dbus::utility::MapperGetSubTreePathsResponse>();
            auto outstandingCalls = std::make_shared<size_t>(subtreePaths.size());

            for (const std::string& p : subtreePaths)
            {
                dbus::utility::getProperty<std::vector<AssociationDefinition>>(
                    "xyz.openbmc_project.FanSensor", p,
                    "xyz.openbmc_project.Association.Definitions", "Associations",
                    [asyncResp, p, validChassisPath, outstandingCalls, filteredSubtreePaths, callback](
                        const boost::system::error_code& ec2,
                        const std::vector<AssociationDefinition>& associations) {

                        auto checkCompletion = [outstandingCalls, filteredSubtreePaths, callback]() {
                            (*outstandingCalls)--;
                            if (*outstandingCalls == 0)
                            {
                                callback(*filteredSubtreePaths);
                            }
                        };

                        if (ec2)
                        {
                            if (ec2.value() != boost::system::errc::io_error && ec2.value() != EBADR)
                            {
                                BMCWEB_LOG_ERROR("DBUS response error {}", ec2);
                                messages::internalError(asyncResp->res);
                            }
                            checkCompletion();
                            return;
                        }


                        // Structured binding works perfectly fine with std::tuple
                        for (const auto& [forward, reverse, endpoint] : associations)
                        {
                            if (endpoint == validChassisPath)
                            {
                                filteredSubtreePaths->push_back(p);
                                break;
                            }
                        }
                        checkCompletion();
                });
            }
        });
}

} // namespace fan_utils
} // namespace redfish
