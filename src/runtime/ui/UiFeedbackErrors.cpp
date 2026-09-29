#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc FeedbackInvalid */
    const ErrorCodeDescriptor FeedbackInvalid{UiDomain,
                                              ErrorCode{"runtime_ui.feedback.invalid"},
                                              ErrorSeverity::Error,
                                              "A semantic Runtime UI feedback outcome is malformed.",
                                              "Use a validated typed action, focus, or control outcome.",
                                              false,
                                              false};
    /** @copydoc FeedbackSourceStale */
    const ErrorCodeDescriptor FeedbackSourceStale{UiDomain,
                                                  ErrorCode{"runtime_ui.feedback.source_stale"},
                                                  ErrorSeverity::Error,
                                                  "Feedback targets a foreign or stale presented owner generation.",
                                                  "Discard the old intent and use the current presented UI owner.",
                                                  false,
                                                  false};
    /** @copydoc FeedbackCapacityExceeded */
    const ErrorCodeDescriptor FeedbackCapacityExceeded{UiDomain,
                                                       ErrorCode{"runtime_ui.feedback.capacity_exceeded"},
                                                       ErrorSeverity::Error,
                                                       "The bounded Runtime UI feedback queue is full.",
                                                       "Drain or discard optional feedback at an owner safe point.",
                                                       false,
                                                       false};
    /** @copydoc FeedbackLifecycleUnavailable */
    const ErrorCodeDescriptor FeedbackLifecycleUnavailable{UiDomain,
                                                           ErrorCode{"runtime_ui.feedback.lifecycle_unavailable"},
                                                           ErrorSeverity::Error,
                                                           "The feedback owner is retiring or stopped.",
                                                           "Create a new feedback queue after Runtime UI reload.",
                                                           false,
                                                           false};
    /** @copydoc FeedbackConsumerFailed */
    const ErrorCodeDescriptor FeedbackConsumerFailed{UiDomain,
                                                     ErrorCode{"runtime_ui.feedback.consumer_failed"},
                                                     ErrorSeverity::Error,
                                                     "The optional host feedback realizer failed.",
                                                     "Keep the UI action result and diagnose host audio/haptics separately.",
                                                     false,
                                                     false};
}  // namespace Horo::Runtime::Ui::UiErrors
