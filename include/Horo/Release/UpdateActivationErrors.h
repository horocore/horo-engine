#pragma once

/**
 * @file UpdateActivationErrors.h
 * @brief Typed identity, recovery, and health failures for update activation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateActivationErrors {
    /** @brief Installation paths or typed version identities violate the activation layout. */
    extern const ErrorCodeDescriptor InvalidLayout;
    /** @brief The active pointer does not identify the declared current version. */
    extern const ErrorCodeDescriptor CurrentMismatch;
    /** @brief An interrupted transition belongs to different versions or has an unknown active pointer. */
    extern const ErrorCodeDescriptor PendingMismatch;
    /** @brief A failed startup probe was rolled back to the verified previous version. */
    extern const ErrorCodeDescriptor HealthFailed;
    /** @brief Restoring the previous active pointer failed; the journal remains for recovery. */
    extern const ErrorCodeDescriptor RollbackFailed;
}  // namespace Horo::Release::UpdateActivationErrors
