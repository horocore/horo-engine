#include "Horo/XR/XRSessionErrors.h"

#include <array>

namespace Horo::XR::XRSessionErrors {
    namespace {
        const ErrorDomainId XRDomain{"horo.xr"};
    }

    const ErrorCodeDescriptor ActivationFailed{
        .domain = XRDomain,
        .code = ErrorCode{"xr.session.activation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR session preparation failed before a new generation could be published.",
        .remediationHint = "Inspect the typed adapter cause; restore the failed dependency and retry complete activation.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ActivationCancelled{
        .domain = XRDomain,
        .code = ErrorCode{"xr.session.activation_cancelled"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "XR session activation was cancelled before publication.",
        .remediationHint = "Start a new activation if the session is still required.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor WrongThread{
        .domain = XRDomain,
        .code = ErrorCode{"xr.session.wrong_thread"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The XR session operation ran outside its control thread.",
        .remediationHint = "Dispatch the operation to the thread that owns the XR session.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor TransitionInvalid{
        .domain = XRDomain,
        .code = ErrorCode{"xr.session.transition_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The runtime event is not legal from the current XR session state.",
        .remediationHint = "Apply native events in order for the exact active generation; do not force state from UI or gameplay.",
        .retryable = false,
        .userActionable = false,
    };

    /** @copydoc Descriptors */
    std::span<const ErrorCodeDescriptor *const> Descriptors() noexcept {
        static constexpr std::array descriptors{&ActivationFailed, &ActivationCancelled, &WrongThread, &TransitionInvalid};
        return descriptors;
    }
}  // namespace Horo::XR::XRSessionErrors
