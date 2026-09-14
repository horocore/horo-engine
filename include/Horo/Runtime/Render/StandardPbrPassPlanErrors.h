#pragma once

/**
 * @file StandardPbrPassPlanErrors.h
 * @brief Stable failures for backend-neutral standard PBR opaque/depth planning.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::StandardPbrPassPlanErrors {
    extern const ErrorCodeDescriptor InvalidRequest;  /**< @brief The selected raster policy or finite bound is invalid. */
    extern const ErrorCodeDescriptor InvalidMaterial; /**< @brief A resident material identity or resource generation is invalid. */
    extern const ErrorCodeDescriptor UnsupportedMaterialClass; /**< @brief A non-opaque material was submitted to the opaque/depth plan. */
    extern const ErrorCodeDescriptor DuplicateMaterial;        /**< @brief One runtime material identity was submitted more than once. */
    extern const ErrorCodeDescriptor CapacityExceeded; /**< @brief The submitted material count exceeds the admitted finite bound. */
    extern const ErrorCodeDescriptor AllocationFailed; /**< @brief Owned plan storage could not be allocated. */
}  // namespace Horo::Render::StandardPbrPassPlanErrors
