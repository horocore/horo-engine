#pragma once

/**
 * @file UpdateDiscoveryErrors.h
 * @brief Stable update-check policy failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateDiscoveryErrors {
    /** @brief Channel, interval or automation policy is malformed or contradictory. */
    extern const ErrorCodeDescriptor InvalidPolicy;
    /** @brief Channel change requires explicit user or administrator action. */
    extern const ErrorCodeDescriptor ChannelChangeRequiresAction;
    /** @brief Automatic scheduling cannot use a clock that predates check history. */
    extern const ErrorCodeDescriptor ClockMovedBackward;
}  // namespace Horo::Release::UpdateDiscoveryErrors
