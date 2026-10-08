#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Runtime::RuntimeErrors {
    extern const ErrorCodeDescriptor InvalidSchedulerConfig;
    extern const ErrorCodeDescriptor InvalidLifecycleState;
    extern const ErrorCodeDescriptor NullParticipant;
    extern const ErrorCodeDescriptor Cancelled;
    extern const ErrorCodeDescriptor UnexpectedException;
    extern const ErrorCodeDescriptor PresentationPrerequisitesMissing;
    extern const ErrorCodeDescriptor PresentationClockGenerationExhausted;
    extern const ErrorCodeDescriptor FixedAttemptIdentityExhausted;
    extern const ErrorCodeDescriptor FrameIdentityExhausted;
    extern const ErrorCodeDescriptor PresentationDurationExhausted;
    extern const ErrorCodeDescriptor DispatchInvalid;
    extern const ErrorCodeDescriptor DispatchReentrant;
    extern const ErrorCodeDescriptor DispatchExhausted;
    extern const ErrorCodeDescriptor DispatchRetired;
    extern const ErrorCodeDescriptor DispatchStorageExhausted;
    extern const ErrorCodeDescriptor SimulationTimingInvalid;
    extern const ErrorCodeDescriptor SimulationTimingStale;
    extern const ErrorCodeDescriptor SimulationTimingCapacity;
    extern const ErrorCodeDescriptor SimulationTimingOverflow;
    extern const ErrorCodeDescriptor SimulationTimingClosed;
}  // namespace Horo::Runtime::RuntimeErrors
