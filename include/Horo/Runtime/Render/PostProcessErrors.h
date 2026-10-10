#pragma once

/** @file PostProcessErrors.h
 * @brief Stable actionable post-process model and graph preparation failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::PostProcessErrors {
    extern const ErrorCodeDescriptor InvalidSettings;   /**< Authored settings contain invalid or unbounded values. */
    extern const ErrorCodeDescriptor InvalidVolume;     /**< A volume identity, shape, weight, override, or position is invalid. */
    extern const ErrorCodeDescriptor InvalidProfile;    /**< A profile identity is duplicate, absent, or invalid. */
    extern const ErrorCodeDescriptor CapacityExceeded;  /**< Authored work exceeds an admitted bound. */
    extern const ErrorCodeDescriptor InvalidGraph;      /**< A graph request has invalid ordering or generation metadata. */
    extern const ErrorCodeDescriptor MissingInput;      /**< A required typed semantic input is absent. */
    extern const ErrorCodeDescriptor IncompatibleInput; /**< Input color, exposure, view, format, or history metadata disagrees. */
    extern const ErrorCodeDescriptor UnsupportedRecipe; /**< No explicitly authored recipe is admitted by capabilities and budgets. */
    extern const ErrorCodeDescriptor AllocationFailed;  /**< Owned preparation storage could not be allocated. */
}  // namespace Horo::Render::PostProcessErrors
