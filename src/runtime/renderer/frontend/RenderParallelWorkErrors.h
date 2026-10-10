#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::Detail::ParallelWorkErrors {
    extern const ErrorCodeDescriptor InvalidLimits;
    extern const ErrorCodeDescriptor CapacityExceeded;
    extern const ErrorCodeDescriptor InvalidPass;
    extern const ErrorCodeDescriptor InvalidGeometry;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor CaptureFailed;
}  // namespace Horo::Render::Detail::ParallelWorkErrors
