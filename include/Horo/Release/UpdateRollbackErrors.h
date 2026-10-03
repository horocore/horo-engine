#pragma once

/**
 * @file UpdateRollbackErrors.h
 * @brief Stable policy and intent errors for last-known-good rollback.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateRollbackErrors {
    /** @brief Requested target is not a prior version of the same product. */
    extern const ErrorCodeDescriptor InvalidTarget;
    /** @brief Downgrade is below the trusted minimum and lacks administrator recovery authority. */
    extern const ErrorCodeDescriptor PolicyDenied;
    /** @brief An explicit downgrade has not been acknowledged by the user. */
    extern const ErrorCodeDescriptor ConfirmationRequired;
}  // namespace Horo::Release::UpdateRollbackErrors
