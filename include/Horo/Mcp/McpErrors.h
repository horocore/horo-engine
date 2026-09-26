#pragma once

/**
 * @file McpErrors.h
 * @brief Stable failures for local MCP session admission, framing, and lifecycle.
 */

#include "Horo/Foundation/ErrorCode.h"

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
}  // namespace Horo::Mcp::McpErrors
