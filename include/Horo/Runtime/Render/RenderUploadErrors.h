#pragma once

/**
 * @file RenderUploadErrors.h
 * @brief Stable typed failures for bounded renderer upload lifecycle operations.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderUploadErrors {
    extern const ErrorCodeDescriptor InvalidConfiguration;
    extern const ErrorCodeDescriptor InvalidDescriptor;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor InvalidRequest;
    extern const ErrorCodeDescriptor InvalidTransition;
    extern const ErrorCodeDescriptor ResultPending;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor TimedOut;
    extern const ErrorCodeDescriptor PayloadSizeMismatch;
    extern const ErrorCodeDescriptor Stopped;
    extern const ErrorCodeDescriptor WrongThread;
}  // namespace Horo::Render::RenderUploadErrors
