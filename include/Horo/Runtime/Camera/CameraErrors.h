#pragma once

/** @file CameraErrors.h
 * @brief Stable camera ownership and scene-resolution error descriptors.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::CameraErrors {
    extern const ErrorCodeDescriptor InvalidContext;    /**< Malformed or unsupported view authority. */
    extern const ErrorCodeDescriptor Closed;            /**< Owner no longer admits proposals. */
    extern const ErrorCodeDescriptor InvalidLease;      /**< Stale, foreign or retired override lease. */
    extern const ErrorCodeDescriptor CapacityExceeded;  /**< Bounded override or hierarchy capacity exceeded. */
    extern const ErrorCodeDescriptor DuplicateClaim;    /**< Claim already belongs to this context. */
    extern const ErrorCodeDescriptor InvalidTarget;     /**< Target is missing, disabled or has invalid camera values. */
    extern const ErrorCodeDescriptor CameraUnavailable; /**< Current base and overrides have no eligible camera. */
    extern const ErrorCodeDescriptor InvalidFrame;      /**< Frame ordering or epoch cannot advance safely. */
}  // namespace Horo::Runtime::CameraErrors
