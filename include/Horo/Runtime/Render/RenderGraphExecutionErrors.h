#pragma once

/**
 * @file RenderGraphExecutionErrors.h
 * @brief Stable typed errors for backend-neutral graph execution compilation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderGraphExecutionErrors {
    extern const ErrorCodeDescriptor UnsupportedWorkload;
    extern const ErrorCodeDescriptor SubmissionObservationUnsupported;
    extern const ErrorCodeDescriptor DeviceIncarnationExhausted;
    /** @brief Retained typed VFX work has no implemented native particle encoder. */
    extern const ErrorCodeDescriptor VfxExecutionUnavailable;
    /** @brief Complete view sorting/draw runs exceed the remaining global budget. */
    extern const ErrorCodeDescriptor VfxDrawBudgetExceeded;
    /** @brief Renderer view storage was not prepared or requested instances exceed its admitted capacity. */
    extern const ErrorCodeDescriptor VfxSchedulingCapacity;
    /** @brief Every prepared view slot remains retained by an outstanding graph/workload reader. */
    extern const ErrorCodeDescriptor VfxViewsRetained;
    extern const ErrorCodeDescriptor AllocationFailed;
    extern const ErrorCodeDescriptor InvalidGraph;
    extern const ErrorCodeDescriptor InvalidQueueTopology;
    extern const ErrorCodeDescriptor InvalidSchedule;
    extern const ErrorCodeDescriptor InvalidSynchronization;
}  // namespace Horo::Render::RenderGraphExecutionErrors
