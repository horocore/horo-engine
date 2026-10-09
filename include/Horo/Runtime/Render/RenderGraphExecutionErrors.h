#pragma once

/**
 * @file RenderGraphExecutionErrors.h
 * @brief Stable typed errors for backend-neutral graph execution compilation.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderGraphExecutionErrors {
    extern const ErrorCodeDescriptor UnsupportedWorkload;
    extern const ErrorCodeDescriptor AllocationFailed;
    extern const ErrorCodeDescriptor InvalidGraph;
    extern const ErrorCodeDescriptor InvalidQueueTopology;
    extern const ErrorCodeDescriptor InvalidSchedule;
    extern const ErrorCodeDescriptor InvalidSynchronization;
}  // namespace Horo::Render::RenderGraphExecutionErrors
