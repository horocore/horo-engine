#pragma once

/**
 * @file McpErrors.h
 * @brief Stable failures for local MCP session admission, framing, and lifecycle.
 */

#include "Horo/Foundation/ErrorCode.h"

#include <nlohmann/json.hpp>

namespace Horo::Mcp {
    /** @brief Encodes bounded typed error identities while omitting private messages and locations.
     * @param error Canonical application error. @return Safe protocol data with code, severity, diagnostics, and causes. */
    [[nodiscard]] nlohmann::json SafeErrorData(const Error &error);
}  // namespace Horo::Mcp

namespace Horo::Mcp::McpErrors {
    /** @brief The controller or a declared session limit is invalid. */
    extern const ErrorCodeDescriptor ConfigurationInvalid;
    /** @brief An explicitly admitted local or embedded caller has invalid identity or authority. */
    extern const ErrorCodeDescriptor AdmissionInvalid;
    /** @brief The host's concurrent-session budget is exhausted. */
    extern const ErrorCodeDescriptor SessionCapacityExceeded;
    /** @brief A session handle is closed, stale, or belongs to another generation. */
    extern const ErrorCodeDescriptor SessionUnavailable;
    /** @brief The host admission barrier is closed for shutdown. */
    extern const ErrorCodeDescriptor ShuttingDown;
    /** @brief A request envelope is malformed or duplicates an active request identity. */
    extern const ErrorCodeDescriptor RequestInvalid;
    /** @brief A session has reached its concurrent request budget. */
    extern const ErrorCodeDescriptor RequestCapacityExceeded;
    /** @brief An input frame or value exceeds a declared resource budget. */
    extern const ErrorCodeDescriptor InputCapacityExceeded;
    /** @brief A controller result exceeds the bounded session result budget. */
    extern const ErrorCodeDescriptor ResultCapacityExceeded;
    /** @brief A request was cooperatively cancelled before producing a result. */
    extern const ErrorCodeDescriptor RequestCancelled;
    /** @brief An accepted request crossed its deadline before producing a result. */
    extern const ErrorCodeDescriptor RequestTimedOut;
    /** @brief A controller callback failed without a typed application error. */
    extern const ErrorCodeDescriptor ControllerFailed;
    /** @brief Shutdown closed admission but callbacks did not drain within the declared wait. */
    extern const ErrorCodeDescriptor DrainTimedOut;
    /** @brief A tool descriptor, schema, or finite bound is malformed or unsupported. */
    extern const ErrorCodeDescriptor ToolDescriptorInvalid;
    /** @brief A candidate registry contains the same stable tool identity more than once. */
    extern const ErrorCodeDescriptor ToolDuplicate;
    /** @brief Replacement changes a published tool's major contract or regresses its version. */
    extern const ErrorCodeDescriptor ToolIncompatible;
    /** @brief The requested tool is absent from the current immutable snapshot. */
    extern const ErrorCodeDescriptor ToolUnavailable;
    /** @brief The admitted session lacks a capability declared by the tool. */
    extern const ErrorCodeDescriptor ToolCapabilityUnavailable;
    /** @brief Input does not satisfy the registered bounded schema. */
    extern const ErrorCodeDescriptor ToolInputInvalid;
    /** @brief An adapter returned data outside its registered bounded output schema. */
    extern const ErrorCodeDescriptor ToolOutputInvalid;
    /** @brief The owner context is not bound to the calling thread. */
    extern const ErrorCodeDescriptor OwnerUnavailable;
    /** @brief Controller queue or active operation budget is full. */
    extern const ErrorCodeDescriptor OperationCapacityExceeded;
    /** @brief Operation is absent, expired, or owned by another session generation. */
    extern const ErrorCodeDescriptor OperationUnavailable;
    /** @brief Session was admitted against a different registry generation. */
    extern const ErrorCodeDescriptor RegistryRevisionStale;
}  // namespace Horo::Mcp::McpErrors
