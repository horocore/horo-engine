#pragma once
/** @file PipelinePreparationErrors.h
 * @brief Stable prewarming policy and lifecycle failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::PipelinePreparationErrors {
    extern const ErrorCodeDescriptor InvalidBudget;         /**< Finite work envelope is invalid. */
    extern const ErrorCodeDescriptor InvalidManifest;       /**< Identity, generation or fallback contract is invalid. */
    extern const ErrorCodeDescriptor MissingCookedArtifact; /**< Packaged compilation cannot synthesize a missing variant. */
    extern const ErrorCodeDescriptor Pending;               /**< Required pipeline has not become resident. */
    extern const ErrorCodeDescriptor StaleCompletion;       /**< Completion is not an exact in-flight operation. */
    extern const ErrorCodeDescriptor Cancelled;             /**< Host cancellation permanently closed admission. */
    extern const ErrorCodeDescriptor Closed;                /**< Owner shut down preparation. */
    extern const ErrorCodeDescriptor AllocationFailed;      /**< Bounded preparation storage could not be allocated. */
}  // namespace Horo::Render::PipelinePreparationErrors
