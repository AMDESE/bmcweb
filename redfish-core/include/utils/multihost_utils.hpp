
#pragma once

#include "async_resp.hpp"

#include <app.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace redfish
{

namespace multihost_utils
{

/**
 * @brief Extracts and validates the HostNumber from the request.
 *
 * This function reads the HostNumber from the JSON request body, validates
 * that it is either 0, 1, or 2, and updates the response accordingly.
 * If HostNumber is missing, it defaults to 0.
 *
 * Expected JSON format:
 * {
 *     "HostNumber": 0
 * }
 *
 * HostNumber values:
 * - 0: Indicates single host mode
 * - 1: Indicates Host 1
 * - 2: Indicates Host 2
 *
 * @param req The incoming request containing the HostNumber.
 * @param asyncResp The asynchronous response object to update the response.
 * @param hostNumberOpt Optional string to store the validated HostNumber.
 */
inline void getHostNumber(const crow::Request& req,
                          const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                          std::optional<std::string>& hostNumberOpt)
{
    std::string hostNumberStr;
    if (!json_util::readJsonAction(req, asyncResp->res, "HostNumber",
                                   hostNumberStr))
    {
        // HostNumber is missing, default to 0
        asyncResp->res.jsonValue["HostNumber"] = 0;
        hostNumberOpt = "0";
        return;
    }
    try
    {
        int hostNumber = std::stoi(hostNumberStr);
        if (hostNumber == 0 || hostNumber == 1 || hostNumber == 2)
        {
            asyncResp->res.jsonValue["HostNumber"] = hostNumber;
            hostNumberOpt = hostNumberStr;
            return;
        }
    }
    catch (const std::invalid_argument& e)
    {
        asyncResp->res.jsonValue["error"] = "Invalid HostNumber format.";
        return;
    }
    catch (const std::out_of_range& e)
    {
        asyncResp->res.jsonValue["error"] = "HostNumber out of range.";
        return;
    }
    asyncResp->res.jsonValue["error"] =
        "Invalid HostNumber. Must be 0, 1, or 2.";
}
} // namespace multihost_utils
} // namespace redfish
