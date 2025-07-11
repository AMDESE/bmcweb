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

#include "http_response.hpp"

#include <boost/url/url_view_base.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

// IWYU pragma: no_include <cstdint.h>
// IWYU pragma: no_forward_declare crow::Response

namespace redfish
{

namespace messages
{

constexpr const char* messageVersionPrefix = "Base.1.11.0.";
constexpr const char* messageAnnotation = "@Message.ExtendedInfo";

/**
 * @brief Moves all error messages from the |source| JSON to |target|
 */
void moveErrorsToErrorJson(nlohmann::json& target, nlohmann::json& source);

/**
 * @brief Formats ResourceInUse message into JSON
 * Message body: "The change to the requested resource failed because the
 * resource is in use or in transition."
 *
 *
 * @returns Message ResourceInUse formatted to JSON */
nlohmann::json resourceInUse();

void resourceInUse(crow::Response& res);

/**
 * @brief Formats MalformedJSON message into JSON
 * Message body: "The request body submitted was malformed JSON and could not be
 * parsed by the receiving service."
 *
 *
 * @returns Message MalformedJSON formatted to JSON */
nlohmann::json malformedJSON();

void malformedJSON(crow::Response& res);

/**
 * @brief Formats ResourceMissingAtURI message into JSON
 * Message body: "The resource at the URI <arg1> was not found."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ResourceMissingAtURI formatted to JSON */
nlohmann::json resourceMissingAtURI(const boost::urls::url_view_base& arg1);

void resourceMissingAtURI(crow::Response& res,
                          const boost::urls::url_view_base& arg1);

/**
 * @brief Formats ActionParameterValueFormatError message into JSON
 * Message body: "The value <arg1> for the parameter <arg2> in the action <arg3>
 * is of a different format than the parameter can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message ActionParameterValueFormatError formatted to JSON */
nlohmann::json actionParameterValueFormatError(const nlohmann::json& arg1,
                                               std::string_view arg2,
                                               std::string_view arg3);

void actionParameterValueFormatError(crow::Response& res,
                                     const nlohmann::json& arg1,
                                     std::string_view arg2,
                                     std::string_view arg3);

/**
 * @brief Formats ActionParameterValueNotInList message into JSON
 * Message body: "The value <arg1> for the parameter <arg2> in the action <arg3>
 * is not in the list of acceptable values."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message ActionParameterValueFormatError formatted to JSON */
nlohmann::json actionParameterValueNotInList(std::string_view arg1,
                                             std::string_view arg2,
                                             std::string_view arg3);

void actionParameterValueNotInList(crow::Response& res, std::string_view arg1,
                                   std::string_view arg2,
                                   std::string_view arg3);

/**
 * @brief Formats InternalError message into JSON
 * Message body: "The request failed due to an internal service error.  The
 * service is still operational."
 *
 *
 * @returns Message InternalError formatted to JSON */
nlohmann::json internalError();

void internalError(crow::Response& res, std::source_location location =
                                            std::source_location::current());

/**
 * @brief Formats UnrecognizedRequestBody message into JSON
 * Message body: "The service detected a malformed request body that it was
 * unable to interpret."
 *
 *
 * @returns Message UnrecognizedRequestBody formatted to JSON */
nlohmann::json unrecognizedRequestBody();

void unrecognizedRequestBody(crow::Response& res);

/**
 * @brief Formats ResourceAtUriUnauthorized message into JSON
 * Message body: "While accessing the resource at <arg1>, the service received
 * an authorization error <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ResourceAtUriUnauthorized formatted to JSON */
nlohmann::json resourceAtUriUnauthorized(const boost::urls::url_view_base& arg1,
                                         std::string_view arg2);

void resourceAtUriUnauthorized(crow::Response& res,
                               const boost::urls::url_view_base& arg1,
                               std::string_view arg2);

/**
 * @brief Formats ActionParameterUnknown message into JSON
 * Message body: "The action <arg1> was submitted with the invalid parameter
 * <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ActionParameterUnknown formatted to JSON */
nlohmann::json actionParameterUnknown(std::string_view arg1,
                                      std::string_view arg2);

void actionParameterUnknown(crow::Response& res, std::string_view arg1,
                            std::string_view arg2);

/**
 * @brief Formats ResourceCannotBeDeleted message into JSON
 * Message body: "The delete request failed because the resource requested
 * cannot be deleted."
 *
 *
 * @returns Message ResourceCannotBeDeleted formatted to JSON */
nlohmann::json resourceCannotBeDeleted();

void resourceCannotBeDeleted(crow::Response& res);

/**
 * @brief Formats PropertyDuplicate message into JSON
 * Message body: "The property <arg1> was duplicated in the request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PropertyDuplicate formatted to JSON */
nlohmann::json propertyDuplicate(std::string_view arg1);

void propertyDuplicate(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats ServiceTemporarilyUnavailable message into JSON
 * Message body: "The service is temporarily unavailable.  Retry in <arg1>
 * seconds."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ServiceTemporarilyUnavailable formatted to JSON */
nlohmann::json serviceTemporarilyUnavailable(std::string_view arg1);

void serviceTemporarilyUnavailable(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats ResourceAlreadyExists message into JSON
 * Message body: "The requested resource of type <arg1> with the property <arg2>
 * with the value <arg3> already exists."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message ResourceAlreadyExists formatted to JSON */
nlohmann::json resourceAlreadyExists(std::string_view arg1,
                                     std::string_view arg2,
                                     std::string_view arg3);

void resourceAlreadyExists(crow::Response& res, std::string_view arg1,
                           std::string_view arg2, std::string_view arg3);

/**
 * @brief Formats AccountForSessionNoLongerExists message into JSON
 * Message body: "The account for the current session has been removed, thus the
 * current session has been removed as well."
 *
 *
 * @returns Message AccountForSessionNoLongerExists formatted to JSON */
nlohmann::json accountForSessionNoLongerExists();

void accountForSessionNoLongerExists(crow::Response& res);

/**
 * @brief Formats CreateFailedMissingReqProperties message into JSON
 * Message body: "The create operation failed because the required property
 * <arg1> was missing from the request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message CreateFailedMissingReqProperties formatted to JSON */
nlohmann::json createFailedMissingReqProperties(std::string_view arg1);

void createFailedMissingReqProperties(crow::Response& res,
                                      std::string_view arg1);

/**
 * @brief Formats PropertyValueFormatError message into JSON
 * Message body: "The value <arg1> for the property <arg2> is of a different
 * format than the property can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueFormatError formatted to JSON */
nlohmann::json propertyValueFormatError(const nlohmann::json& arg1,
                                        std::string_view arg2);

void propertyValueFormatError(crow::Response& res, const nlohmann::json& arg1,
                              std::string_view arg2);

/**
 * @brief Formats PropertyValueNotInList message into JSON
 * Message body: "The value <arg1> for the property <arg2> is not in the list of
 * acceptable values."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueNotInList formatted to JSON */
nlohmann::json propertyValueNotInList(const nlohmann::json& arg1,
                                      std::string_view arg2);

void propertyValueNotInList(crow::Response& res, const nlohmann::json& arg1,
                            std::string_view arg2);
/**
 * @brief Formats PropertyValueOutOfRange message into JSON
 * Message body: "The value '%1' for the property %2 is not in the supported
 * range of acceptable values."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueExternalConflict formatted to JSON */
nlohmann::json propertyValueOutOfRange(const nlohmann::json& arg1,
                                       std::string_view arg2);

void propertyValueOutOfRange(crow::Response& res, const nlohmann::json& arg1,
                             std::string_view arg2);

/**
 * @brief Formats ResourceAtUriInUnknownFormat message into JSON
 * Message body: "The resource at <arg1> is in a format not recognized by the
 * service."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ResourceAtUriInUnknownFormat formatted to JSON */
nlohmann::json
    resourceAtUriInUnknownFormat(const boost::urls::url_view_base& arg1);

void resourceAtUriInUnknownFormat(crow::Response& res,
                                  const boost::urls::url_view_base& arg1);

/**
 * @brief Formats ServiceDisabled message into JSON
 * Message body: "The operation failed because the service at <arg1> is disabled
 * and " cannot accept requests."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ServiceDisabled formatted to JSON */
nlohmann::json serviceDisabled(std::string_view arg1);

void serviceDisabled(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats ServiceInUnknownState message into JSON
 * Message body: "The operation failed because the service is in an unknown
 * state and can no longer take incoming requests."
 *
 *
 * @returns Message ServiceInUnknownState formatted to JSON */
nlohmann::json serviceInUnknownState();

void serviceInUnknownState(crow::Response& res);

/**
 * @brief Formats EventSubscriptionLimitExceeded message into JSON
 * Message body: "The event subscription failed due to the number of
 * simultaneous subscriptions exceeding the limit of the implementation."
 *
 *
 * @returns Message EventSubscriptionLimitExceeded formatted to JSON */
nlohmann::json eventSubscriptionLimitExceeded();

void eventSubscriptionLimitExceeded(crow::Response& res);

/**
 * @brief Formats ActionParameterMissing message into JSON
 * Message body: "The action <arg1> requires the parameter <arg2> to be present
 * in the request body."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ActionParameterMissing formatted to JSON */
nlohmann::json actionParameterMissing(std::string_view arg1,
                                      std::string_view arg2);

void actionParameterMissing(crow::Response& res, std::string_view arg1,
                            std::string_view arg2);

/**
 * @brief Formats StringValueTooLong message into JSON
 * Message body: "The string <arg1> exceeds the length limit <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message StringValueTooLong formatted to JSON */
nlohmann::json stringValueTooLong(std::string_view arg1, int arg2);

void stringValueTooLong(crow::Response& res, std::string_view arg1, int arg2);

/**
 * @brief Formats SessionTerminated message into JSON
 * Message body: "The session was successfully terminated."
 *
 *
 * @returns Message SessionTerminated formatted to JSON */
nlohmann::json sessionTerminated();

void sessionTerminated(crow::Response& res);

/**
 * @brief Formats SubscriptionTerminated message into JSON
 * Message body: "The event subscription has been terminated."
 *
 *
 * @returns Message SubscriptionTerminated formatted to JSON */
nlohmann::json subscriptionTerminated();

void subscriptionTerminated(crow::Response& res);

/**
 * @brief Formats ResourceTypeIncompatible message into JSON
 * Message body: "The @odata.type of the request body <arg1> is incompatible
 * with the @odata.type of the resource which is <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ResourceTypeIncompatible formatted to JSON */
nlohmann::json resourceTypeIncompatible(std::string_view arg1,
                                        std::string_view arg2);

void resourceTypeIncompatible(crow::Response& res, std::string_view arg1,
                              std::string_view arg2);

/**
 * @brief Formats ResetRequired message into JSON
 * Message body: "In order to complete the operation, a component reset is
 * required with the Reset action URI '<arg1>' and ResetType '<arg2>'."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ResetRequired formatted to JSON */
nlohmann::json resetRequired(const boost::urls::url_view_base& arg1,
                             std::string_view arg2);

void resetRequired(crow::Response& res, const boost::urls::url_view_base& arg1,
                   std::string_view arg2);

/**
 * @brief Formats ChassisPowerStateOnRequired message into JSON
 * Message body: "The Chassis with Id '<arg1>' requires to be powered on to
 * perform this request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ChassisPowerStateOnRequired formatted to JSON */
nlohmann::json chassisPowerStateOnRequired(std::string_view arg1);

void chassisPowerStateOnRequired(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats ChassisPowerStateOffRequired message into JSON
 * Message body: "The Chassis with Id '<arg1>' requires to be powered off to
 * perform this request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ChassisPowerStateOffRequired formatted to JSON */
nlohmann::json chassisPowerStateOffRequired(std::string_view arg1);

void chassisPowerStateOffRequired(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats PropertyValueConflict message into JSON
 * Message body: "The property '<arg1>' could not be written because its value
 * would conflict with the value of the '<arg2>' property."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueConflict formatted to JSON */
nlohmann::json propertyValueConflict(std::string_view arg1,
                                     std::string_view arg2);

void propertyValueConflict(crow::Response& res, std::string_view arg1,
                           std::string_view arg2);

/**
 * @brief Formats PropertyValueResourceConflict message into JSON
 * Message body: "The property '%1' with the requested value of '%2' could
 * not be written because the value conflicts with the state or configuration
 * of the resource at '%3'."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message PropertyValueResourceConflict to JSON */
nlohmann::json
    propertyValueResourceConflict(std::string_view arg1,
                                  const nlohmann::json& arg2,
                                  const boost::urls::url_view_base& arg3);

void propertyValueResourceConflict(crow::Response& res, std::string_view arg1,
                                   const nlohmann::json& arg2,
                                   const boost::urls::url_view_base& arg3);

/**
 * @brief Formats PropertyValueExternalConflict message into JSON
 * Message body: "The property '%1' with the requested value of '%2' could not
 * be written because the value is not available due to a configuration
 * conflict."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueExternalConflict formatted to JSON */
nlohmann::json propertyValueExternalConflict(std::string_view arg1,
                                             const nlohmann::json& arg2);

void propertyValueExternalConflict(crow::Response& res, std::string_view arg1,
                                   const nlohmann::json& arg2);

/**
 * @brief Formats PropertyValueIncorrect message into JSON
 * Message body: "The property '<arg1>' with the requested value of '<arg2>'
 * could not be written because the value does not meet the constraints of the
 * implementation."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueIncorrect formatted to JSON */
nlohmann::json propertyValueIncorrect(std::string_view arg1,
                                      const nlohmann::json& arg2);

void propertyValueIncorrect(crow::Response& res, std::string_view arg1,
                            const nlohmann::json& arg2);

/**
 * @brief Formats ResourceCreationConflict message into JSON
 * Message body: "The resource could not be created.  The service has a resource
 * at URI '<arg1>' that conflicts with the creation request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ResourceCreationConflict formatted to JSON */
nlohmann::json resourceCreationConflict(const boost::urls::url_view_base& arg1);

void resourceCreationConflict(crow::Response& res,
                              const boost::urls::url_view_base& arg1);

/**
 * @brief Formats MaximumErrorsExceeded message into JSON
 * Message body: "Too many errors have occurred to report them all."
 *
 *
 * @returns Message MaximumErrorsExceeded formatted to JSON */
nlohmann::json maximumErrorsExceeded();

void maximumErrorsExceeded(crow::Response& res);

/**
 * @brief Formats PreconditionFailed message into JSON
 * Message body: "The ETag supplied did not match the ETag required to change
 * this resource."
 *
 *
 * @returns Message PreconditionFailed formatted to JSON */
nlohmann::json preconditionFailed();

void preconditionFailed(crow::Response& res);

/**
 * @brief Formats PreconditionRequired message into JSON
 * Message body: "A precondition header or annotation is required to change this
 * resource."
 *
 *
 * @returns Message PreconditionRequired formatted to JSON */
nlohmann::json preconditionRequired();

void preconditionRequired(crow::Response& res);

/**
 * @brief Formats OperationFailed message into JSON
 * Message body: "An error occurred internal to the service as part of the
 * overall request.  Partial results may have been returned."
 *
 *
 * @returns Message OperationFailed formatted to JSON */
nlohmann::json operationFailed();

void operationFailed(crow::Response& res);

/**
 * @brief Formats OperationTimeout message into JSON
 * Message body: "A timeout internal to the service occurred as part of the
 * request.  Partial results may have been returned."
 *
 *
 * @returns Message OperationTimeout formatted to JSON */
nlohmann::json operationTimeout();

void operationTimeout(crow::Response& res);

/**
 * @brief Formats PropertyValueTypeError message into JSON
 * Message body: "The value <arg1> for the property <arg2> is of a different
 * type than the property can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueTypeError formatted to JSON */
nlohmann::json propertyValueTypeError(const nlohmann::json& arg1,
                                      std::string_view arg2);

void propertyValueTypeError(crow::Response& res, const nlohmann::json& arg1,
                            std::string_view arg2);

/**
 * @brief Formats ResourceNotFound message into JSON
 * Message body: "The requested resource of type <arg1> named <arg2> was not
 * found."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ResourceNotFound formatted to JSON */
nlohmann::json resourceNotFound(std::string_view arg1, std::string_view arg2);

void resourceNotFound(crow::Response& res, std::string_view arg1,
                      std::string_view arg2);

/**
 * @brief Formats CouldNotEstablishConnection message into JSON
 * Message body: "The service failed to establish a Connection with the URI
 * <arg1>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message CouldNotEstablishConnection formatted to JSON */
nlohmann::json
    couldNotEstablishConnection(const boost::urls::url_view_base& arg1);

void couldNotEstablishConnection(crow::Response& res,
                                 const boost::urls::url_view_base& arg1);

/**
 * @brief Formats PropertyNotWritable message into JSON
 * Message body: "The property <arg1> is a read only property and cannot be
 * assigned a value."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PropertyNotWritable formatted to JSON */
nlohmann::json propertyNotWritable(std::string_view arg1);

void propertyNotWritable(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats QueryParameterValueTypeError message into JSON
 * Message body: "The value <arg1> for the query parameter <arg2> is of a
 * different type than the parameter can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message QueryParameterValueTypeError formatted to JSON */
nlohmann::json queryParameterValueTypeError(const nlohmann::json& arg1,
                                            std::string_view arg2);

void queryParameterValueTypeError(crow::Response& res,
                                  const nlohmann::json& arg1,
                                  std::string_view arg2);

/**
 * @brief Formats ServiceShuttingDown message into JSON
 * Message body: "The operation failed because the service is shutting down and
 * can no longer take incoming requests."
 *
 *
 * @returns Message ServiceShuttingDown formatted to JSON */
nlohmann::json serviceShuttingDown();

void serviceShuttingDown(crow::Response& res);

/**
 * @brief Formats ActionParameterDuplicate message into JSON
 * Message body: "The action <arg1> was submitted with more than one value for
 * the parameter <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ActionParameterDuplicate formatted to JSON */
nlohmann::json actionParameterDuplicate(std::string_view arg1,
                                        std::string_view arg2);

void actionParameterDuplicate(crow::Response& res, std::string_view arg1,
                              std::string_view arg2);

/**
 * @brief Formats ActionParameterNotSupported message into JSON
 * Message body: "The parameter <arg1> for the action <arg2> is not supported on
 * the target resource."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ActionParameterNotSupported formatted to JSON */
nlohmann::json actionParameterNotSupported(std::string_view arg1,
                                           std::string_view arg2);

void actionParameterNotSupported(crow::Response& res, std::string_view arg1,
                                 std::string_view arg2);

/**
 * @brief Formats SourceDoesNotSupportProtocol message into JSON
 * Message body: "The other end of the Connection at <arg1> does not support the
 * specified protocol <arg2>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message SourceDoesNotSupportProtocol formatted to JSON */
nlohmann::json
    sourceDoesNotSupportProtocol(const boost::urls::url_view_base& arg1,
                                 std::string_view arg2);

void sourceDoesNotSupportProtocol(crow::Response& res,
                                  const boost::urls::url_view_base& arg1,
                                  std::string_view arg2);

/**
 * @brief Formats StrictAccountTypes message into JSON
 * Message body: Indicates the request failed because a set of `AccountTypes` or
 * `OEMAccountTypes` was not accepted while `StrictAccountTypes` is set to `true
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message StrictAccountTypes formatted to JSON */
nlohmann::json strictAccountTypes(std::string_view arg1);

void strictAccountTypes(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats AccountRemoved message into JSON
 * Message body: "The account was successfully removed."
 *
 *
 * @returns Message AccountRemoved formatted to JSON */
nlohmann::json accountRemoved();

void accountRemoved(crow::Response& res);

/**
 * @brief Formats AccessDenied message into JSON
 * Message body: "While attempting to establish a Connection to <arg1>, the
 * service denied access."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message AccessDenied formatted to JSON */
nlohmann::json accessDenied(const boost::urls::url_view_base& arg1);

void accessDenied(crow::Response& res, const boost::urls::url_view_base& arg1);

/**
 * @brief Formats QueryNotSupported message into JSON
 * Message body: "Querying is not supported by the implementation."
 *
 *
 * @returns Message QueryNotSupported formatted to JSON */
nlohmann::json queryNotSupported();

void queryNotSupported(crow::Response& res);

/**
 * @brief Formats CreateLimitReachedForResource message into JSON
 * Message body: "The create operation failed because the resource has reached
 * the limit of possible resources."
 *
 *
 * @returns Message CreateLimitReachedForResource formatted to JSON */
nlohmann::json createLimitReachedForResource();

void createLimitReachedForResource(crow::Response& res);

/**
 * @brief Formats GeneralError message into JSON
 * Message body: "A general error has occurred. See ExtendedInfo for more
 * information."
 *
 *
 * @returns Message GeneralError formatted to JSON */
nlohmann::json generalError();

void generalError(crow::Response& res);

/**
 * @brief Formats Success message into JSON
 * Message body: "Successfully Completed Request"
 *
 *
 * @returns Message Success formatted to JSON */
nlohmann::json success();

void success(crow::Response& res);

/**
 * @brief Formats Created message into JSON
 * Message body: "The resource has been created successfully"
 *
 *
 * @returns Message Created formatted to JSON */
nlohmann::json created();

void created(crow::Response& res);

/**
 * @brief Formats NoOperation message into JSON
 * Message body: "The request body submitted contain no data to act upon and
 * no changes to the resource took place."
 *
 *
 * @returns Message NoOperation formatted to JSON */
nlohmann::json noOperation();

void noOperation(crow::Response& res);

/**
 * @brief Formats PropertyUnknown message into JSON
 * Message body: "The property <arg1> is not in the list of valid properties for
 * the resource."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PropertyUnknown formatted to JSON */
nlohmann::json propertyUnknown(std::string_view arg1);

void propertyUnknown(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats NoValidSession message into JSON
 * Message body: "There is no valid session established with the
 * implementation."
 *
 *
 * @returns Message NoValidSession formatted to JSON */
nlohmann::json noValidSession();

void noValidSession(crow::Response& res);

/**
 * @brief Formats InvalidObject message into JSON
 * Message body: "The object at <arg1> is invalid."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message InvalidObject formatted to JSON */
nlohmann::json invalidObject(const boost::urls::url_view_base& arg1);

void invalidObject(crow::Response& res, const boost::urls::url_view_base& arg1);

/**
 * @brief Formats ResourceInStandby message into JSON
 * Message body: "The request could not be performed because the resource is in
 * standby."
 *
 *
 * @returns Message ResourceInStandby formatted to JSON */
nlohmann::json resourceInStandby();

void resourceInStandby(crow::Response& res);

/**
 * @brief Formats ActionParameterValueTypeError message into JSON
 * Message body: "The value <arg1> for the parameter <arg2> in the action <arg3>
 * is of a different type than the parameter can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message ActionParameterValueTypeError formatted to JSON */
nlohmann::json actionParameterValueTypeError(const nlohmann::json& arg1,
                                             std::string_view arg2,
                                             std::string_view arg3);

void actionParameterValueTypeError(crow::Response& res,
                                   const nlohmann::json& arg1,
                                   std::string_view arg2,
                                   std::string_view arg3);

/**
 * @brief Formats ActionParameterValueError message into JSON
 * Message body: "Indicates that a parameter was given an invalid value."
 *  The value for the parameter %1 in the action %2 is invalid.
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message ActionParameterValueError formatted to JSON */
nlohmann::json actionParameterValueError(const nlohmann::json& arg1,
                                         std::string_view arg2);

void actionParameterValueError(crow::Response& res, const nlohmann::json& arg1,
                               std::string_view arg2);

/**
 * @brief Formats SessionLimitExceeded message into JSON
 * Message body: "The session establishment failed due to the number of
 * simultaneous sessions exceeding the limit of the implementation."
 *
 *
 * @returns Message SessionLimitExceeded formatted to JSON */
nlohmann::json sessionLimitExceeded();

void sessionLimitExceeded(crow::Response& res);

/**
 * @brief Formats ActionNotSupported message into JSON
 * Message body: "The action <arg1> is not supported by the resource."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ActionNotSupported formatted to JSON */
nlohmann::json actionNotSupported(std::string_view arg1);

void actionNotSupported(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats InvalidIndex message into JSON
 * Message body: "The index <arg1> is not a valid offset into the array."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message InvalidIndex formatted to JSON */
nlohmann::json invalidIndex(int64_t arg1);

void invalidIndex(crow::Response& res, int64_t arg1);

/**
 * @brief Formats EmptyJSON message into JSON
 * Message body: "The request body submitted contained an empty JSON object and
 * the service is unable to process it."
 *
 *
 * @returns Message EmptyJSON formatted to JSON */
nlohmann::json emptyJSON();

void emptyJSON(crow::Response& res);

/**
 * @brief Formats QueryNotSupportedOnResource message into JSON
 * Message body: "Querying is not supported on the requested resource."
 *
 *
 * @returns Message QueryNotSupportedOnResource formatted to JSON */
nlohmann::json queryNotSupportedOnResource();

void queryNotSupportedOnResource(crow::Response& res);

/**
 * @brief Formats QueryNotSupportedOnOperation message into JSON
 * Message body: "Querying is not supported with the requested operation."
 *
 *
 * @returns Message QueryNotSupportedOnOperation formatted to JSON */
nlohmann::json queryNotSupportedOnOperation();

void queryNotSupportedOnOperation(crow::Response& res);

/**
 * @brief Formats QueryCombinationInvalid message into JSON
 * Message body: "Two or more query parameters in the request cannot be used
 * together."
 *
 *
 * @returns Message QueryCombinationInvalid formatted to JSON */
nlohmann::json queryCombinationInvalid();

void queryCombinationInvalid(crow::Response& res);

/**
 * @brief Formats EventBufferExceeded message into JSON
 * Message body: "Indicates undelivered events may have been lost due to a lack
 * of buffer space in the service."
 *
 *
 * @returns Message QueryCombinationInvalid formatted to JSON */
nlohmann::json eventBufferExceeded();

void eventBufferExceeded(crow::Response& res);

/**
 * @brief Formats InsufficientPrivilege message into JSON
 * Message body: "There are insufficient privileges for the account or
 * credentials associated with the current session to perform the requested
 * operation."
 *
 *
 * @returns Message InsufficientPrivilege formatted to JSON */
nlohmann::json insufficientPrivilege();

void insufficientPrivilege(crow::Response& res);

/**
 * @brief Formats PropertyValueModified message into JSON
 * Message body: "The property <arg1> was assigned the value <arg2> due to
 * modification by the service."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message PropertyValueModified formatted to JSON */
nlohmann::json propertyValueModified(std::string_view arg1,
                                     const nlohmann::json& arg2);

void propertyValueModified(crow::Response& res, std::string_view arg1,
                           const nlohmann::json& arg2);

/**
 * @brief Formats AccountNotModified message into JSON
 * Message body: "The account modification request failed."
 *
 *
 * @returns Message AccountNotModified formatted to JSON */
nlohmann::json accountNotModified();

void accountNotModified(crow::Response& res);

/**
 * @brief Formats QueryParameterValueFormatError message into JSON
 * Message body: "The value <arg1> for the parameter <arg2> is of a different
 * format than the parameter can accept."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message QueryParameterValueFormatError formatted to JSON */

nlohmann::json queryParameterValueFormatError(const nlohmann::json& arg1,
                                              std::string_view arg2);

void queryParameterValueFormatError(crow::Response& res,
                                    const nlohmann::json& arg1,
                                    std::string_view arg2);

/**
 * @brief Formats PropertyMissing message into JSON
 * Message body: "The property <arg1> is a required property and must be
 * included in the request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PropertyMissing formatted to JSON */
nlohmann::json propertyMissing(std::string_view arg1);

void propertyMissing(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats PropertyNotUpdated message into JSON
 * Message body: "The property <arg1> is a required property and must be
 * included in the request."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PropertyNotUpdated formatted to JSON */
nlohmann::json propertyNotUpdated(std::string_view arg1);

void propertyNotUpdated(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats ResourceExhaustion message into JSON
 * Message body: "The resource <arg1> was unable to satisfy the request due to
 * unavailability of resources."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message ResourceExhaustion formatted to JSON */
nlohmann::json resourceExhaustion(std::string_view arg1);

void resourceExhaustion(crow::Response& res, std::string_view arg1);

/**
 * @brief Formats AccountModified message into JSON
 * Message body: "The account was successfully modified."
 *
 *
 * @returns Message AccountModified formatted to JSON */
nlohmann::json accountModified();

void accountModified(crow::Response& res);

/**
 * @brief Formats QueryParameterOutOfRange message into JSON
 * Message body: "The value <arg1> for the query parameter <arg2> is out of
 * range <arg3>."
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 * @param[in] arg3 Parameter of message that will replace %3 in its body.
 *
 * @returns Message QueryParameterOutOfRange formatted to JSON */
nlohmann::json queryParameterOutOfRange(std::string_view arg1,
                                        std::string_view arg2,
                                        std::string_view arg3);

void queryParameterOutOfRange(crow::Response& res, std::string_view arg1,
                              std::string_view arg2, std::string_view arg3);

/**
 * @brief Formats PasswordChangeRequired message into JSON
 * Message body: The password provided for this account must be changed
 * before access is granted.  PATCH the 'Password' property for this
 * account located at the target URI '%1' to complete this process.
 *
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 *
 * @returns Message PasswordChangeRequired formatted to JSON */

nlohmann::json passwordChangeRequired(const boost::urls::url_view_base& arg1);

void passwordChangeRequired(crow::Response& res,
                            const boost::urls::url_view_base& arg1);

/**
 * @brief Formats InvalidUpload message into JSON
 * Message body: Invalid file uploaded to %1: %2.*
 * @param[in] arg1 Parameter of message that will replace %1 in its body.
 * @param[in] arg2 Parameter of message that will replace %2 in its body.
 *
 * @returns Message InvalidUpload formatted to JSON */
nlohmann::json invalidUpload(std::string_view arg1, std::string_view arg2);

void invalidUpload(crow::Response& res, std::string_view arg1,
                   std::string_view arg2);

/**
 * @brief Formats InsufficientStorage message into JSON
 * Message body: "Insufficient storage or memory available to complete the
 *  request."
 * @returns Message InsufficientStorage formatted to JSON */
nlohmann::json insufficientStorage();

void insufficientStorage(crow::Response& res);

/**
 * @brief Formats OperationNotAllowed message into JSON
 * Message body: "he HTTP method is not allowed on this resource."
 * @returns Message OperationNotAllowed formatted to JSON */
nlohmann::json operationNotAllowed();

void operationNotAllowed(crow::Response& res);

/**
 * @brief Formats ArraySizeTooLong message into JSON
 * Message body: "Indicates that a string value passed to the given resource
 * exceeded its length limit."
 * @returns Message ArraySizeTooLong formatted to JSON */
nlohmann::json arraySizeTooLong(std::string_view property, uint64_t length);

void arraySizeTooLong(crow::Response& res, std::string_view property,
                      uint64_t length);

} // namespace messages

} // namespace redfish
