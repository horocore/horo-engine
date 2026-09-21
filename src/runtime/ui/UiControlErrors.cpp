#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc ControlDescriptorInvalid */
    const ErrorCodeDescriptor
        ControlDescriptorInvalid{UiDomain,
                                 ErrorCode{"runtime_ui.control_descriptor.invalid"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI interactive-control descriptor is invalid.",
                                 "Use one supported typed control, exact owner evidence, finite values, and bounded payloads.",
                                 false,
                                 true};
    /** @copydoc ControlInputInvalid */
    const ErrorCodeDescriptor ControlInputInvalid{UiDomain,
                                                  ErrorCode{"runtime_ui.control_input.invalid"},
                                                  ErrorSeverity::Error,
                                                  "The Runtime UI control input is malformed or incompatible with its control.",
                                                  "Submit normalized owner-addressed edges with a known kind, source and bounded payload.",
                                                  false,
                                                  false};
    /** @copydoc ControlSourceStale */
    const ErrorCodeDescriptor ControlSourceStale{UiDomain,
                                                 ErrorCode{"runtime_ui.control.source_stale"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI control input belongs to another owner, element or presented revision.",
                                                 "Retarget the input against the latest successfully presented control generation.",
                                                 true,
                                                 false};
    /** @copydoc ControlDefaultPending */
    const ErrorCodeDescriptor ControlDefaultPending{UiDomain,
                                                    ErrorCode{"runtime_ui.control.default_pending"},
                                                    ErrorSeverity::Error,
                                                    "A Runtime UI control received input before its routed default action was resolved.",
                                                    "Apply or suppress the pending default action before admitting the next input edge.",
                                                    false,
                                                    false};
    /** @copydoc ControlDefaultInvalid */
    const ErrorCodeDescriptor
        ControlDefaultInvalid{UiDomain,
                              ErrorCode{"runtime_ui.control.default_invalid"},
                              ErrorSeverity::Error,
                              "The Runtime UI control default action cannot be represented by its typed payload.",
                              "Keep authored control payloads within the fixed argument bound and use finite typed values.",
                              false,
                              true};
    /** @copydoc ControlCapacityExceeded */
    const ErrorCodeDescriptor ControlCapacityExceeded{UiDomain,
                                                      ErrorCode{"runtime_ui.control.capacity_exceeded"},
                                                      ErrorSeverity::Error,
                                                      "The Runtime UI control state or text input exceeds its fixed capacity.",
                                                      "Reduce the bounded text or command value before submitting the control input.",
                                                      false,
                                                      true};
    /** @copydoc ControlSequenceInvalid */
    const ErrorCodeDescriptor ControlSequenceInvalid{UiDomain,
                                                     ErrorCode{"runtime_ui.control.sequence_invalid"},
                                                     ErrorSeverity::Error,
                                                     "The Runtime UI control event sequence or repeat tick is stale or exhausted.",
                                                     "Submit strictly ordered owner evidence and never wrap a control sequence or tick.",
                                                     true,
                                                     false};
    /** @copydoc ControlLifecycleUnavailable */
    const ErrorCodeDescriptor
        ControlLifecycleUnavailable{UiDomain,
                                    ErrorCode{"runtime_ui.control.lifecycle_unavailable"},
                                    ErrorSeverity::Error,
                                    "The Runtime UI interactive-control state machine is retiring or stopped.",
                                    "Close input before retirement and create a new control for a replacement generation.",
                                    false,
                                    false};
}  // namespace Horo::Runtime::Ui::UiErrors
