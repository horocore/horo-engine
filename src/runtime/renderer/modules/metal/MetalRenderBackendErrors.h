#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::MetalBackendErrors {
    extern const ErrorCodeDescriptor AlreadyInitialized;
    extern const ErrorCodeDescriptor AdapterNotFound;
    extern const ErrorCodeDescriptor AdapterUnavailable;
    extern const ErrorCodeDescriptor CommandQueueCreationFailed;
    extern const ErrorCodeDescriptor FrameActive;
    extern const ErrorCodeDescriptor FrameAlreadyActive;
    extern const ErrorCodeDescriptor FrameTokenExhausted;
    extern const ErrorCodeDescriptor FrameTokenMismatch;
    extern const ErrorCodeDescriptor InvalidConfig;
    extern const ErrorCodeDescriptor InvalidDeviceFacts;
    extern const ErrorCodeDescriptor InvalidExecutionPlan;
    extern const ErrorCodeDescriptor InvalidExtent;
    extern const ErrorCodeDescriptor InvalidFrameDescriptor;
    extern const ErrorCodeDescriptor NoActiveFrame;
    extern const ErrorCodeDescriptor NotInitialized;
    extern const ErrorCodeDescriptor PresentationInUse;
    extern const ErrorCodeDescriptor PresentationUnsupported;
    extern const ErrorCodeDescriptor RequiredFormatUnsupported;
    extern const ErrorCodeDescriptor ResourceCreationFailed;
    extern const ErrorCodeDescriptor ResourceIdentityInvalid;
    extern const ErrorCodeDescriptor UnsupportedFramesInFlight;
    extern const ErrorCodeDescriptor UnsupportedDeviceFamily;
    extern const ErrorCodeDescriptor UnsupportedHost;
    extern const ErrorCodeDescriptor UnsupportedPassKind;
    extern const ErrorCodeDescriptor UnsupportedResourceOperation;
    extern const ErrorCodeDescriptor StaleAdapterSnapshot;
}  // namespace Horo::Render::MetalBackendErrors
