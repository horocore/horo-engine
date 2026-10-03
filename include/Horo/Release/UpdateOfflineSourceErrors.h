#pragma once

/**
 * @file UpdateOfflineSourceErrors.h
 * @brief Stable source-admission and offline-media diagnostics.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Release::UpdateOfflineSourceErrors {
    /** @brief Managed source identity, precedence, or channel policy is invalid. */
    extern const ErrorCodeDescriptor InvalidPolicy;
    /** @brief Configured local media or a required file is unavailable. */
    extern const ErrorCodeDescriptor Unavailable;
    /** @brief A local path is linked, unsafe, or outside the configured source root. */
    extern const ErrorCodeDescriptor UnsafePath;
    /** @brief Source package bytes do not match the exact signed manifest record. */
    extern const ErrorCodeDescriptor MirrorMismatch;
    /** @brief Signed metadata is current but offers no newer compatible ZIP package. */
    extern const ErrorCodeDescriptor NoUpdate;
}  // namespace Horo::Release::UpdateOfflineSourceErrors
