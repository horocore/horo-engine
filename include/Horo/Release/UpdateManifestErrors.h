#pragma once

/**
 * @file UpdateManifestErrors.h
 * @brief Stable failures for signed update metadata admission.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateManifestErrors {
    /** @brief Metadata is malformed, non-canonical, oversized, or contradicts its package evidence. */
    extern const ErrorCodeDescriptor Invalid;
    /** @brief Metadata is expired, not yet valid, or older than installed monotonic trust state. */
    extern const ErrorCodeDescriptor Stale;
    /** @brief Product, channel, host target, or updater capability does not match. */
    extern const ErrorCodeDescriptor Incompatible;
    /** @brief Version ordering violates the explicit downgrade and anti-rollback policy. */
    extern const ErrorCodeDescriptor Rollback;
}  // namespace Horo::Release::UpdateManifestErrors
