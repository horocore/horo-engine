#pragma once
/** @file RenderGraphInspectionErrors.h
 * @brief Stable actionable inspection capture, export and publication failures.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderGraphInspectionErrors {
    extern const ErrorCodeDescriptor InvalidSource;
    extern const ErrorCodeDescriptor InvalidLimits;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor AllocationFailed;
    extern const ErrorCodeDescriptor WrongThread;
    extern const ErrorCodeDescriptor Closed;
    extern const ErrorCodeDescriptor StalePublication;
}  // namespace Horo::Render::RenderGraphInspectionErrors
