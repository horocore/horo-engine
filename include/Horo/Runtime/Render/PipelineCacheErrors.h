#pragma once

/**
 * @file PipelineCacheErrors.h
 * @brief Stable failures for native pipeline-cache identity and serialized-blob admission.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::PipelineCacheErrors {
    extern const ErrorCodeDescriptor InvalidLimits;        /**< @brief Caller bounds are zero or exceed hard safety limits. */
    extern const ErrorCodeDescriptor InvalidCompatibility; /**< @brief A compatibility dimension is absent or malformed. */
    extern const ErrorCodeDescriptor IdentityUnavailable;  /**< @brief Canonical identity storage could not be allocated. */
    extern const ErrorCodeDescriptor PayloadTooLarge;      /**< @brief Native payload exceeds the admitted finite bound. */
    extern const ErrorCodeDescriptor UnsupportedVersion;   /**< @brief Serialized bytes use an unsupported schema version. */
    extern const ErrorCodeDescriptor CorruptBlob;          /**< @brief Serialized structure or payload integrity is invalid. */
    extern const ErrorCodeDescriptor IncompatibleBlob;     /**< @brief Blob key does not match the active compatibility key. */
    extern const ErrorCodeDescriptor AllocationFailed;     /**< @brief Owned serialized or loaded payload allocation failed. */
}  // namespace Horo::Render::PipelineCacheErrors
