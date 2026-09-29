#pragma once

/**
 * @file UpdateRetentionErrors.h
 * @brief Stable disk-budget planning failures for installed update versions.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateRetentionErrors {
    /** @brief Installed-version snapshot has missing pins, duplicate IDs, invalid roles, or overflowing sizes. */
    extern const ErrorCodeDescriptor InvalidSnapshot;
    /** @brief Active and last-known-good versions alone exceed the configured budget. */
    extern const ErrorCodeDescriptor ProtectedBudgetExceeded;
    /** @brief Installed records, authenticated ownership evidence, or private tree disagrees with the cleanup request. */
    extern const ErrorCodeDescriptor UnsafeCleanup;
}  // namespace Horo::Release::UpdateRetentionErrors
