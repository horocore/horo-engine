#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::OpenGLBackendErrors {
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
    extern const ErrorCodeDescriptor InvalidRegistration;
    extern const ErrorCodeDescriptor UnsupportedApiFamily;
    extern const ErrorCodeDescriptor UnsupportedVersion;
    extern const ErrorCodeDescriptor UnsupportedProfile;
    extern const ErrorCodeDescriptor MissingRequiredEntryPoints;
    extern const ErrorCodeDescriptor InvalidCapabilities;
    extern const ErrorCodeDescriptor WrongThread;
    extern const ErrorCodeDescriptor PresentationInUse;
    extern const ErrorCodeDescriptor UnsupportedPassKind;
    extern const ErrorCodeDescriptor UnsupportedResourceOperation;
}  // namespace Horo::Render::OpenGLBackendErrors
