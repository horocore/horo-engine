#include "Horo/Audio/AudioErrors.h"

namespace Horo::Audio::AudioErrors {
    namespace {
        const ErrorDomainId AudioDomain{"horo.audio"};
    }

    const ErrorCodeDescriptor StreamUnderrun{.domain = AudioDomain,
                                             .code = ErrorCode{"audio.stream.underrun"},
                                             .defaultSeverity = ErrorSeverity::Warning,
                                             .summary = "The stream could not provide the admitted frames in time.",
                                             .remediationHint =
                                                 "Render admitted silence for the missing frames and refill outside the callback.",
                                             .retryable = true};
    const ErrorCodeDescriptor StreamReadFailed{.domain = AudioDomain,
                                               .code = ErrorCode{"audio.stream.read_failed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The active stream source could not be read.",
                                               .remediationHint =
                                                   "Keep the current epoch valid and retry bounded stream fill on control work.",
                                               .retryable = true};
    const ErrorCodeDescriptor StreamCapacityExceeded{.domain = AudioDomain,
                                                     .code = ErrorCode{"audio.stream.capacity_exceeded"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "A stream exceeds its admitted buffer or worker capacity.",
                                                     .remediationHint = "Reduce stream demand or prepare a larger validated capacity.",
                                                     .userActionable = true};
    const ErrorCodeDescriptor
        QueueSaturated{.domain = AudioDomain,
                       .code = ErrorCode{"audio.queue.saturated"},
                       .defaultSeverity = ErrorSeverity::Warning,
                       .summary = "The bounded ordinary audio command queue is full.",
                       .remediationHint = "Retain the request and retry at a later control safe point; preserve reserved critical work.",
                       .retryable = true};
    const ErrorCodeDescriptor GraphBuildFailed{.domain = AudioDomain,
                                               .code = ErrorCode{"audio.graph.build_failed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "A candidate mixer graph could not be prepared.",
                                               .remediationHint =
                                                   "Reject the candidate and retain the active graph until a valid replacement is ready.",
                                               .userActionable = true};
    const ErrorCodeDescriptor GraphStaleCandidate{.domain = AudioDomain,
                                                  .code = ErrorCode{"audio.graph.stale_candidate"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The prepared mixer graph no longer matches the requested revision.",
                                                  .remediationHint =
                                                      "Discard the stale candidate and prepare the latest revision outside the callback.",
                                                  .retryable = true};
    const ErrorCodeDescriptor ProviderUnavailable{.domain = AudioDomain,
                                                  .code = ErrorCode{"audio.provider.unavailable"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The selected audio provider is unavailable.",
                                                  .remediationHint =
                                                      "Apply the declared required or optional provider policy; never silently fall back.",
                                                  .userActionable = true};
    const ErrorCodeDescriptor ProviderFailed{.domain = AudioDomain,
                                             .code = ErrorCode{"audio.provider.failed"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The selected audio provider failed after activation.",
                                             .remediationHint =
                                                 "Retain the last valid epoch and request explicit provider recovery policy.",
                                             .retryable = true};
    const ErrorCodeDescriptor MiddlewareBindingInvalid{.domain = AudioDomain,
                                                       .code = ErrorCode{"audio.middleware.binding_invalid"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "A middleware binding manifest is invalid.",
                                                       .remediationHint =
                                                           "Correct exact event, parameter, adapter and bank identities before activation.",
                                                       .userActionable = true};
    const ErrorCodeDescriptor MiddlewareBankUnavailable{.domain = AudioDomain,
                                                        .code = ErrorCode{"audio.middleware.bank_unavailable"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "A required middleware bank is unavailable.",
                                                        .remediationHint =
                                                            "Reject the incomplete candidate and retain the active bank generation.",
                                                        .userActionable = true};
    const ErrorCodeDescriptor MiddlewareFailed{.domain = AudioDomain,
                                               .code = ErrorCode{"audio.middleware.failed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The selected middleware backend failed.",
                                               .remediationHint = "Quiesce the output epoch and request host-owned recovery policy.",
                                               .retryable = true};
    const ErrorCodeDescriptor DeviceFormatUnsupported{.domain = AudioDomain,
                                                      .code = ErrorCode{"audio.device.format_unsupported"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The candidate output format is unsupported.",
                                                      .remediationHint = "Reject the candidate without replacing the active device epoch.",
                                                      .userActionable = true};
    const ErrorCodeDescriptor DeviceLost{.domain = AudioDomain,
                                         .code = ErrorCode{"audio.device.lost"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "The active audio output device was lost.",
                                         .remediationHint = "Close device-dependent admission and attempt bounded explicit recovery.",
                                         .retryable = true};
    const ErrorCodeDescriptor CallbackFault{.domain = AudioDomain,
                                            .code = ErrorCode{"audio.callback.fault"},
                                            .defaultSeverity = ErrorSeverity::Critical,
                                            .summary = "The active audio callback reported an unrecoverable fault.",
                                            .remediationHint =
                                                "Quiesce and retain the epoch until callback detachment is proven; notify the host.",
                                            .retryable = false};
}  // namespace Horo::Audio::AudioErrors
