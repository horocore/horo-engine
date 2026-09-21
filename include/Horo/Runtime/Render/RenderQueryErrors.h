#pragma once

/**
 * @file RenderQueryErrors.h
 * @brief Stable typed failures for bounded renderer timestamp-query operations.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::RenderQueryErrors {
    extern const ErrorCodeDescriptor InvalidConfiguration;
    extern const ErrorCodeDescriptor InvalidDescriptor;
    extern const ErrorCodeDescriptor Unsupported;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor InvalidRequest;
    extern const ErrorCodeDescriptor InvalidTransition;
    extern const ErrorCodeDescriptor ResultPending;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor TimedOut;
    extern const ErrorCodeDescriptor TimestampInvalid;
    extern const ErrorCodeDescriptor Stopped;
    extern const ErrorCodeDescriptor WrongThread;
}  // namespace Horo::Render::RenderQueryErrors
