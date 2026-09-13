#pragma once

/**
 * @file ShaderReflectionErrors.h
 * @brief Stable failures for final shader reflection, layout, interface, and source-map validation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::ShaderReflectionErrors {
    extern const ErrorCodeDescriptor InvalidLimits;          /**< @brief Caller bounds are zero or exceed hard safety limits. */
    extern const ErrorCodeDescriptor InvalidReflection;      /**< @brief Reflection values or collection ordering are malformed. */
    extern const ErrorCodeDescriptor ManifestMismatch;       /**< @brief Final reflection disagrees with the declared logical interface. */
    extern const ErrorCodeDescriptor TargetMappingInvalid;   /**< @brief A target binding is absent, inconsistent, or unverifiable. */
    extern const ErrorCodeDescriptor NativeBindingCollision; /**< @brief Active resources overlap one native target slot. */
    extern const ErrorCodeDescriptor LayoutMismatch;         /**< @brief Parameter packing is invalid, overlapping, or inconsistent. */
    extern const ErrorCodeDescriptor StageInterfaceMismatch; /**< @brief Producer and consumer stage interfaces do not link exactly. */
    extern const ErrorCodeDescriptor SourceMapInvalid;       /**< @brief Source mappings overlap, escape bounds, or lose provenance. */
    extern const ErrorCodeDescriptor AllocationFailed;       /**< @brief Canonical reflection storage could not be allocated. */
}  // namespace Horo::Render::ShaderReflectionErrors
