#pragma once

/**
 * @file PlatformRequestErrors.h
 * @brief Stable failures owned by the platform request-record foundation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::PlatformServices::RequestErrors {
    /** @brief The request store configuration or a supplied generation is invalid. */
    extern const ErrorCodeDescriptor InvalidConfiguration;
    /** @brief The bounded active-request or observer capacity is exhausted. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief The request identity belongs to another or an obsolete frontend generation. */
    extern const ErrorCodeDescriptor Stale;
    /** @brief The retained terminal observation has expired. */
    extern const ErrorCodeDescriptor Expired;
    /** @brief The requested lifecycle transition is not legal from the current state. */
    extern const ErrorCodeDescriptor InvalidTransition;
    /** @brief The frontend request store no longer admits work. */
    extern const ErrorCodeDescriptor FrontendUnavailable;
    /** @brief Provider evidence drain was called outside the composed engine thread. */
    extern const ErrorCodeDescriptor WrongThread;
    /** @brief Provider drain was entered during another drain or observer callback. */
    extern const ErrorCodeDescriptor ReentrantDrain;
    /** @brief An accepted request reached acknowledged cancellation. */
    extern const ErrorCodeDescriptor Cancelled;
    /** @brief An accepted request reached its frontend-owned monotonic deadline. */
    extern const ErrorCodeDescriptor TimedOut;
}  // namespace Horo::PlatformServices::RequestErrors
