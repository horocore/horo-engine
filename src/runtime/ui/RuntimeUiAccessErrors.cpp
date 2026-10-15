#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId Domain{"horo.runtime_ui"};
    }

    /** @copydoc CapabilityUnavailable */
    const ErrorCodeDescriptor CapabilityUnavailable{Domain,
                                                    ErrorCode{"runtime_ui.capability.unavailable"},
                                                    ErrorSeverity::Error,
                                                    "The requested Runtime UI responsibility is unavailable.",
                                                    "Compose the exact canonical owner explicitly.",
                                                    false,
                                                    false};
    /** @copydoc CapabilityDenied */
    const ErrorCodeDescriptor CapabilityDenied{Domain,
                                               ErrorCode{"runtime_ui.capability.denied"},
                                               ErrorSeverity::Error,
                                               "Runtime UI permission was not granted.",
                                               "Request an explicit host policy grant.",
                                               false,
                                               false};
    /** @copydoc CapabilityRevoked */
    const ErrorCodeDescriptor CapabilityRevoked{Domain,
                                                ErrorCode{"runtime_ui.capability.revoked"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI scope has been revoked or replaced.",
                                                "Acquire a new host-issued scope.",
                                                false,
                                                false};
    /** @copydoc CapabilityWrongThread */
    const ErrorCodeDescriptor CapabilityWrongThread{Domain,
                                                    ErrorCode{"runtime_ui.capability.wrong_thread"},
                                                    ErrorSeverity::Error,
                                                    "Runtime UI access is restricted to its owner thread.",
                                                    "Queue host work on the UI owner lane.",
                                                    false,
                                                    false};
    /** @copydoc CapabilityBusy */
    const ErrorCodeDescriptor CapabilityBusy{Domain,
                                             ErrorCode{"runtime_ui.capability.busy"},
                                             ErrorSeverity::Error,
                                             "Runtime UI dispatch cannot be reentered.",
                                             "Submit work after the current callback returns.",
                                             false,
                                             false};
    /** @copydoc CapabilityUnsupported */
    const ErrorCodeDescriptor CapabilityUnsupported{Domain,
                                                    ErrorCode{"runtime_ui.capability.unsupported"},
                                                    ErrorSeverity::Error,
                                                    "The request exceeds the admitted Runtime UI bridge contract.",
                                                    "Use supported typed values within the declared finite bounds.",
                                                    false,
                                                    false};
}  // namespace Horo::Runtime::Ui::UiErrors
