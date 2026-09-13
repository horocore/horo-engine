#pragma once

/**
 * @file RenderReadbackErrors.h
 * @brief Stable typed failures for bounded renderer readback lifecycle operations.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderReadbackErrors {
    /** @brief Queue owner identity or finite bounds are invalid. */
    extern const ErrorCodeDescriptor InvalidConfiguration;
    /** @brief Request source, range, alignment, or timeout policy is invalid. */
    extern const ErrorCodeDescriptor InvalidDescriptor;
    /** @brief Bounded metadata, staging, or retained-result capacity is exhausted. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief Request identity is malformed, foreign, stale, or acknowledged. */
    extern const ErrorCodeDescriptor InvalidRequest;
    /** @brief Requested lifecycle transition is not valid from the current state. */
    extern const ErrorCodeDescriptor InvalidTransition;
    /** @brief Consumer attempted acquisition before asynchronous completion. */
    extern const ErrorCodeDescriptor ResultPending;
    /** @brief Request was cooperatively cancelled and will publish no result. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief Caller-owned finite deadline elapsed before completion. */
    extern const ErrorCodeDescriptor TimedOut;
    /** @brief Backend mapping differs from the exact admitted byte count. */
    extern const ErrorCodeDescriptor MappingSizeMismatch;
    /** @brief New producer admission is disabled during shutdown. */
    extern const ErrorCodeDescriptor Stopped;
}  // namespace Horo::Render::RenderReadbackErrors
