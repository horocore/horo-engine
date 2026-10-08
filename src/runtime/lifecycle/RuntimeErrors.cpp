#include "RuntimeErrors.h"

namespace Horo::Runtime::RuntimeErrors {
    namespace {
        constexpr auto kError = ErrorSeverity::Error;
        const ErrorDomainId kDomain{"horo.runtime"};
    }  // namespace

    const ErrorCodeDescriptor InvalidSchedulerConfig{kDomain, ErrorCode{"runtime.scheduler.invalid_config"}, kError,
                                                     "The frame scheduler configuration is invalid.",
                                                     "Use positive fixed-step, frame-delta, and catch-up limits."};
    const ErrorCodeDescriptor InvalidLifecycleState{kDomain, ErrorCode{"runtime.lifecycle.invalid_state"}, kError,
                                                    "The runtime lifecycle operation is invalid in its current state.",
                                                    "Follow the documented lifecycle state transitions."};
    const ErrorCodeDescriptor NullParticipant{kDomain, ErrorCode{"runtime.lifecycle.null_participant"}, kError,
                                              "A null runtime participant cannot be registered.",
                                              "Supply an owned runtime lifecycle participant."};
    const ErrorCodeDescriptor Cancelled{kDomain, ErrorCode{"runtime.host.cancelled"}, ErrorSeverity::Info,
                                        "Runtime execution was cancelled.", "Complete orderly host shutdown."};
    const ErrorCodeDescriptor UnexpectedException{kDomain, ErrorCode{"runtime.lifecycle.unexpected_exception"}, ErrorSeverity::Critical,
                                                  "A runtime participant threw an unexpected exception.",
                                                  "Fix the participant to return a typed Result instead."};
    const ErrorCodeDescriptor
        PresentationPrerequisitesMissing{kDomain, ErrorCode{"runtime.scheduler.presentation_prerequisites_missing"},
                                         ErrorSeverity::Critical,
                                         "Presentation was reached without successful extraction, execution, and GUI phases.",
                                         "Preserve canonical phase ordering and completion tracking."};
    const ErrorCodeDescriptor
        PresentationClockGenerationExhausted{kDomain, ErrorCode{"runtime.scheduler.presentation_clock_generation_exhausted"}, kError,
                                             "The presentation clock baseline generation cannot advance without wrapping.",
                                             "Create a new host-owned presentation clock namespace after orderly retirement."};
    const ErrorCodeDescriptor FixedAttemptIdentityExhausted{kDomain, ErrorCode{"runtime.scheduler.fixed_attempt_identity_exhausted"},
                                                            kError, "The fixed dispatch attempt identity cannot advance without wrapping.",
                                                            "Retire this scheduler and create a new host-owned clock source."};
    const ErrorCodeDescriptor FrameIdentityExhausted{kDomain, ErrorCode{"runtime.scheduler.frame_identity_exhausted"}, kError,
                                                     "The host frame identity cannot advance without wrapping.",
                                                     "Retire this scheduler and create a new host-owned presentation source."};
    const ErrorCodeDescriptor PresentationDurationExhausted{kDomain, ErrorCode{"runtime.scheduler.presentation_duration_exhausted"}, kError,
                                                            "The cumulative admitted presentation duration cannot advance safely.",
                                                            "Retire this scheduler and create a new host-owned clock source."};
    const ErrorCodeDescriptor SimulationTimingInvalid{kDomain, ErrorCode{"runtime.simulation_timing.invalid"}, kError,
                                                      "The timing command is invalid for its owner thread or policy.",
                                                      "Use the actual host control and a valid positive rate or paused step policy."};
    const ErrorCodeDescriptor SimulationTimingStale{kDomain, ErrorCode{"runtime.simulation_timing.stale"}, kError,
                                                    "The timing command read no longer names the current host admission revision.",
                                                    "Read the actual current host policy before retrying the command."};
    const ErrorCodeDescriptor SimulationTimingCapacity{kDomain, ErrorCode{"runtime.simulation_timing.capacity"}, kError,
                                                       "The bounded timing request storage has no reusable slot.",
                                                       "Release completed receipts or pause leases before admitting more requests."};
    const ErrorCodeDescriptor SimulationTimingOverflow{kDomain, ErrorCode{"runtime.simulation_timing.overflow"}, kError,
                                                       "The simulation duration or timing identity cannot advance safely.",
                                                       "Preserve the active policy and retire exhausted host identities."};
    const ErrorCodeDescriptor SimulationTimingClosed{kDomain, ErrorCode{"runtime.simulation_timing.closed"}, kError,
                                                     "The host timing command capability is retired.",
                                                     "Release retained receipts and leases without issuing new commands."};
    const ErrorCodeDescriptor DispatchInvalid{kDomain, ErrorCode{"runtime.dispatch.invalid"}, kError,
                                              "Dispatch requires the actual scheduler owner thread and initialized issuer.",
                                              "Use the application-bound producer capability on its owner thread."};
    const ErrorCodeDescriptor DispatchReentrant{kDomain, ErrorCode{"runtime.dispatch.reentrant"}, kError,
                                                "A scheduler callback cannot recursively dispatch another frame.",
                                                "Queue work for the next owner frame instead of reentering dispatch."};
    const ErrorCodeDescriptor DispatchExhausted{kDomain, ErrorCode{"runtime.dispatch.exhausted"}, kError,
                                                "The dispatch ordinal cannot advance without wrapping.",
                                                "Retire this scheduler and create a new host source."};
    const ErrorCodeDescriptor DispatchRetired{kDomain, ErrorCode{"runtime.dispatch.retired"}, kError,
                                              "This scheduler dispatch issuer is retired.",
                                              "Release retained evidence without admitting another frame."};
    const ErrorCodeDescriptor DispatchStorageExhausted{kDomain, ErrorCode{"runtime.dispatch.storage_exhausted"}, kError,
                                                       "Producer storage could not be allocated during scheduler construction.",
                                                       "Retry construction only after resolving load-time memory pressure."};
}  // namespace Horo::Runtime::RuntimeErrors
