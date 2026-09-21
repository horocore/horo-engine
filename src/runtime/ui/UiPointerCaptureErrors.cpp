#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc PointerCaptureInvalid */
    const ErrorCodeDescriptor
        PointerCaptureInvalid{UiDomain,
                              ErrorCode{"runtime_ui.pointer_capture.invalid"},
                              ErrorSeverity::Error,
                              "The Runtime UI pointer-capture request or cancellation reason is malformed.",
                              "Provide a non-zero pointer, known button, exact input context, and complete route evidence.",
                              false,
                              false};
    /** @copydoc PointerCaptureSourceStale */
    const ErrorCodeDescriptor
        PointerCaptureSourceStale{UiDomain,
                                  ErrorCode{"runtime_ui.pointer_capture.source_stale"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI pointer-capture source no longer matches the active tree owner.",
                                  "Retarget against the current runtime instance, canvas, tree, and element generation.",
                                  true,
                                  false};
    /** @copydoc PointerCaptureInteractionStale */
    const ErrorCodeDescriptor
        PointerCaptureInteractionStale{UiDomain,
                                       ErrorCode{"runtime_ui.pointer_capture.interaction_stale"},
                                       ErrorSeverity::Error,
                                       "The Runtime UI pointer-capture interaction generation was not presented or is no longer current.",
                                       "Use the last successfully presented interaction snapshot before capturing the pointer.",
                                       true,
                                       false};
    /** @copydoc PointerCaptureBusy */
    const ErrorCodeDescriptor PointerCaptureBusy{UiDomain,
                                                 ErrorCode{"runtime_ui.pointer_capture.busy"},
                                                 ErrorSeverity::Error,
                                                 "The requested Runtime UI pointer is already captured in this input context.",
                                                 "Release or cancel the existing capture before starting another gesture for this pointer.",
                                                 true,
                                                 false};
    /** @copydoc PointerCaptureCapacityExceeded */
    const ErrorCodeDescriptor
        PointerCaptureCapacityExceeded{UiDomain,
                                       ErrorCode{"runtime_ui.pointer_capture.capacity_exceeded"},
                                       ErrorSeverity::Error,
                                       "Every bounded Runtime UI pointer-capture slot is occupied or retired.",
                                       "Release completed or cancelled capture leases before admitting another pointer gesture.",
                                       true,
                                       false};
    /** @copydoc PointerCaptureLifecycleUnavailable */
    const ErrorCodeDescriptor
        PointerCaptureLifecycleUnavailable{UiDomain,
                                           ErrorCode{"runtime_ui.pointer_capture.lifecycle_unavailable"},
                                           ErrorSeverity::Error,
                                           "The Runtime UI pointer-capture store is retiring or stopped.",
                                           "Close the old capture generation and create a store for the active input context generation.",
                                           false,
                                           false};
}  // namespace Horo::Runtime::Ui::UiErrors
