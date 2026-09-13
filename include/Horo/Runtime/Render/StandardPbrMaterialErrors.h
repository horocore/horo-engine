#pragma once

/**
 * @file StandardPbrMaterialErrors.h
 * @brief Stable failures for standard PBR material admission, packing, and residency.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::StandardPbrMaterialErrors {
    extern const ErrorCodeDescriptor InvalidLimits;      /**< @brief Caller-provided finite bounds are invalid. */
    extern const ErrorCodeDescriptor InvalidDescriptor;  /**< @brief Semantic values, modes, features, or quality policy are invalid. */
    extern const ErrorCodeDescriptor UnsupportedQuality; /**< @brief Selected quality or required features were not explicitly admitted. */
    extern const ErrorCodeDescriptor
        InvalidResidentResource; /**< @brief A pipeline, texture, sampler, or generation identity is invalid. */
    extern const ErrorCodeDescriptor
        TextureBindingMismatch;                          /**< @brief Texture roles are duplicate or disagree with the declared features. */
    extern const ErrorCodeDescriptor ReflectionMismatch; /**< @brief Final reflection cannot pack the standard PBR parameter contract. */
    extern const ErrorCodeDescriptor
        ParameterBufferTooLarge;                       /**< @brief Reflected target layout exceeds the admitted parameter-byte bound. */
    extern const ErrorCodeDescriptor AllocationFailed; /**< @brief Owned runtime material storage could not be allocated. */
}  // namespace Horo::Render::StandardPbrMaterialErrors
