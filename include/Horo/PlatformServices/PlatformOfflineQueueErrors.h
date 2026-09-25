#pragma once

/**
 * @file PlatformOfflineQueueErrors.h
 * @brief Stable provider-neutral failures for bounded offline intent policy.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::PlatformServices::OfflineQueueErrors {
    /** @brief Queue limits are invalid or exceed engine hard maxima. */
    extern const ErrorCodeDescriptor InvalidConfiguration;
    /** @brief An intent, lane, or operation payload is malformed. */
    extern const ErrorCodeDescriptor InvalidIntent;
    /** @brief Active or retained intent capacity is exhausted. */
    extern const ErrorCodeDescriptor CapacityExceeded;
    /** @brief A caller reused an intent identity with different canonical content. */
    extern const ErrorCodeDescriptor IdentityConflict;
    /** @brief The handle belongs to another queue generation or is malformed. */
    extern const ErrorCodeDescriptor Stale;
    /** @brief The intent expired or its terminal observation was compacted. */
    extern const ErrorCodeDescriptor Expired;
    /** @brief The requested queue state transition is invalid. */
    extern const ErrorCodeDescriptor InvalidTransition;
    /** @brief The supplied monotonic timestamp moved backward. */
    extern const ErrorCodeDescriptor ClockMovedBackward;
    /** @brief Queue shutdown has closed admission, dispatch, and resumption. */
    extern const ErrorCodeDescriptor QueueUnavailable;
}  // namespace Horo::PlatformServices::OfflineQueueErrors
