#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Input::Errors {
    extern const ErrorCodeDescriptor CollectionInvalid;
    extern const ErrorCodeDescriptor CollectionStale;
    extern const ErrorCodeDescriptor CollectionCapacity;
    extern const ErrorCodeDescriptor CollectionWrongThread;
    extern const ErrorCodeDescriptor ProfileInvalidSchema;
    extern const ErrorCodeDescriptor ProfileMalformed;
    extern const ErrorCodeDescriptor ProfileReadFailed;
    extern const ErrorCodeDescriptor ProfileDirectoryCreationFailed;
    extern const ErrorCodeDescriptor ProfileWriteFailed;
    extern const ErrorCodeDescriptor ProfilePromotionFailed;
    extern const ErrorCodeDescriptor CaptureInactiveContext;
    extern const ErrorCodeDescriptor CaptureBusy;
    extern const ErrorCodeDescriptor ActionMapValidationFailed;
    extern const ErrorCodeDescriptor ProfileValidationFailed;
}  // namespace Horo::Input::Errors
