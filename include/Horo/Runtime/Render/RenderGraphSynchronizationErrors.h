#pragma once

/** @file RenderGraphSynchronizationErrors.h
 * @brief Stable typed errors for render-graph synchronization synthesis.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderGraphSynchronizationErrors {
    extern const ErrorCodeDescriptor AllocationFailed;
    extern const ErrorCodeDescriptor DuplicateInitialState;
    extern const ErrorCodeDescriptor InvalidInitialState;
    /** @brief An external final-state obligation is duplicate, unused, malformed or foreign. */
    extern const ErrorCodeDescriptor InvalidExternalRelease;
    extern const ErrorCodeDescriptor InvalidQueueTopology;
    extern const ErrorCodeDescriptor InvalidSchedule;
    extern const ErrorCodeDescriptor MissingInitialState;
    extern const ErrorCodeDescriptor UnexpectedInitialState;
    extern const ErrorCodeDescriptor UndefinedRead;
    extern const ErrorCodeDescriptor UnsupportedState;
}  // namespace Horo::Render::RenderGraphSynchronizationErrors
