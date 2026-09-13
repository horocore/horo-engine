#pragma once

/**
 * @file ShaderPermutationErrors.h
 * @brief Stable failures for bounded shader permutation selection.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::ShaderPermutationErrors {
    extern const ErrorCodeDescriptor InvalidLimits;          /**< @brief Caller bounds are zero or exceed engine hard limits. */
    extern const ErrorCodeDescriptor InvalidModel;           /**< @brief The model version, feature, or variant bound is invalid. */
    extern const ErrorCodeDescriptor NonCanonicalInput;      /**< @brief Features, masks, or specialization values are not ordered. */
    extern const ErrorCodeDescriptor UnsupportedPermutation; /**< @brief The requested compile-time feature mask was not admitted. */
    extern const ErrorCodeDescriptor InvalidRequest;         /**< @brief A required logical identity is absent or malformed. */
    extern const ErrorCodeDescriptor InvalidSpecialization;  /**< @brief An override is undeclared, mismatched, or malformed. */
    extern const ErrorCodeDescriptor AllocationFailed;       /**< @brief Bounded result storage could not be allocated. */
}  // namespace Horo::Render::ShaderPermutationErrors
