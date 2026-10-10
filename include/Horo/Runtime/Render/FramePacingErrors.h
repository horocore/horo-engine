#pragma once
/** @file FramePacingErrors.h
 * @brief Stable actionable errors for host frame pacing and native presentation evidence.
 */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::FramePacingErrors {
    extern const ErrorCodeDescriptor InvalidPolicy;
    extern const ErrorCodeDescriptor InvalidSurface;
    extern const ErrorCodeDescriptor StaleEvidence;
    extern const ErrorCodeDescriptor InvalidClock;
    extern const ErrorCodeDescriptor InvalidNativeTiming;
    extern const ErrorCodeDescriptor NativeTimingUnsupported;
    extern const ErrorCodeDescriptor WrongThread;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor Stopped;
}  // namespace Horo::Render::FramePacingErrors
