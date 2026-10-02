#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc AsyncActionCapacityExceeded */
    const ErrorCodeDescriptor
        AsyncActionCapacityExceeded{UiDomain,
                                    ErrorCode{"runtime_ui.async_action.capacity_exceeded"},
                                    ErrorSeverity::Warning,
                                    "All asynchronous action slots are retained or leased.",
                                    "Release terminal retention and retire completion/cancellation leases before retrying.",
                                    true,
                                    false};
    /** @copydoc AsyncActionBusy */
    const ErrorCodeDescriptor AsyncActionBusy{UiDomain,
                                              ErrorCode{"runtime_ui.async_action.busy"},
                                              ErrorSeverity::Warning,
                                              "The asynchronous action is still pending.",
                                              "Complete or cancel the pending operation before retrying.",
                                              true,
                                              false};
    /** @copydoc AsyncActionProgressInvalid */
    const ErrorCodeDescriptor AsyncActionProgressInvalid{UiDomain,
                                                         ErrorCode{"runtime_ui.async_action.progress_invalid"},
                                                         ErrorSeverity::Error,
                                                         "Asynchronous action progress is invalid or regressed.",
                                                         "Use monotonic phases and progress in the inclusive 0 to 1000 range.",
                                                         false,
                                                         false};
    /** @copydoc AsyncActionAlreadyTerminal */
    const ErrorCodeDescriptor AsyncActionAlreadyTerminal{UiDomain,
                                                         ErrorCode{"runtime_ui.async_action.already_terminal"},
                                                         ErrorSeverity::Warning,
                                                         "The asynchronous action already has its terminal result.",
                                                         "Discard late completion and progress from the retired operation.",
                                                         false,
                                                         false};
    /** @copydoc AsyncActionFailureInvalid */
    const ErrorCodeDescriptor AsyncActionFailureInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.async_action.failure_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The asynchronous action failure is invalid or exceeds its diagnostic budget.",
                                                        "Supply a bounded immutable typed error prepared by the operation owner.",
                                                        false,
                                                        false};
}  // namespace Horo::Runtime::Ui::UiErrors
