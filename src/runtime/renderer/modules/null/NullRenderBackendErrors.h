#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::NullBackendErrors {
    extern const ErrorCodeDescriptor AlreadyInitialized;
    extern const ErrorCodeDescriptor FrameActive;
    extern const ErrorCodeDescriptor FrameAlreadyActive;
    extern const ErrorCodeDescriptor FrameTokenExhausted;
    extern const ErrorCodeDescriptor FrameTokenMismatch;
    extern const ErrorCodeDescriptor InvalidConfig;
    extern const ErrorCodeDescriptor InvalidExecutionPlan;
    extern const ErrorCodeDescriptor InvalidExtent;
    extern const ErrorCodeDescriptor InvalidFrameDescriptor;
    extern const ErrorCodeDescriptor NoActiveFrame;
    extern const ErrorCodeDescriptor NotInitialized;
    extern const ErrorCodeDescriptor UnsupportedPassKind;
    extern const ErrorCodeDescriptor PresentationUnsupported;
    extern const ErrorCodeDescriptor ResourceInstanceExhausted;
    extern const ErrorCodeDescriptor UnsupportedResourceOperation;
}  // namespace Horo::Render::NullBackendErrors
