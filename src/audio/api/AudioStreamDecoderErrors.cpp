#include "Horo/Audio/AudioStreamDecoderErrors.h"

namespace Horo::Audio::AudioStreamDecoderErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.audio"};
    }

    const ErrorCodeDescriptor Invalid{kDomain,
                                      ErrorCode{"audio.stream_decoder.invalid"},
                                      ErrorSeverity::Error,
                                      "The runtime stream decoder specification or request is invalid.",
                                      "Supply an exact format, positive bounded block size and valid provider operations.",
                                      false,
                                      true};
    const ErrorCodeDescriptor CapacityExceeded{kDomain,
                                               ErrorCode{"audio.stream_decoder.capacity_exceeded"},
                                               ErrorSeverity::Error,
                                               "The stream decoder exceeds an admitted frame, channel or working-memory bound.",
                                               "Reduce the request or prepare a larger validated worker-side capacity.",
                                               false,
                                               true};
    const ErrorCodeDescriptor SeekUnsupported{kDomain,
                                              ErrorCode{"audio.stream_decoder.seek_unsupported"},
                                              ErrorSeverity::Error,
                                              "The selected decoder does not advertise seeking.",
                                              "Use sequential decoding or select a seek-capable provider.",
                                              false,
                                              true};
    const ErrorCodeDescriptor Cancelled{kDomain,
                                        ErrorCode{"audio.stream_decoder.cancelled"},
                                        ErrorSeverity::Warning,
                                        "The stream decode operation was cancelled.",
                                        "Discard its candidate output and release the session after worker completion.",
                                        false,
                                        false};
    const ErrorCodeDescriptor ProviderFailed{kDomain,
                                             ErrorCode{"audio.stream_decoder.provider_failed"},
                                             ErrorSeverity::Error,
                                             "The selected runtime decoder failed without an owned typed result.",
                                             "Retain the previous live stream generation and inspect the provider failure.",
                                             false,
                                             false};
    const ErrorCodeDescriptor ProtocolViolation{kDomain,
                                                ErrorCode{"audio.stream_decoder.protocol_violation"},
                                                ErrorSeverity::Error,
                                                "The decoder reported frames or end-of-stream inconsistent with its declared source.",
                                                "Reject the candidate and repair or replace the provider.",
                                                false,
                                                false};
    const ErrorCodeDescriptor LifecycleUnavailable{kDomain,
                                                   ErrorCode{"audio.stream_decoder.lifecycle_unavailable"},
                                                   ErrorSeverity::Error,
                                                   "The decoder session no longer accepts worker operations.",
                                                   "Create a new session after failure, cancellation or closure.",
                                                   false,
                                                   false};
}  // namespace Horo::Audio::AudioStreamDecoderErrors
