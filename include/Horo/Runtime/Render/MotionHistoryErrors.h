#pragma once

/**
 * @file MotionHistoryErrors.h
 * @brief Stable typed error descriptors for render motion-history tracking.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::MotionHistoryErrors {
    extern const ErrorCodeDescriptor AllocationFailed;
    extern const ErrorCodeDescriptor FrameAlreadyPending;
    extern const ErrorCodeDescriptor InvalidFrame;
    extern const ErrorCodeDescriptor InvalidLimits;
    extern const ErrorCodeDescriptor InvalidRequest;
    extern const ErrorCodeDescriptor TrackerStopped;
    extern const ErrorCodeDescriptor WrongThread;
}  // namespace Horo::Render::MotionHistoryErrors
