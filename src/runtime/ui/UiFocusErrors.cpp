#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc FocusInvalid */
    const ErrorCodeDescriptor FocusInvalid{UiDomain,
                                           ErrorCode{"runtime_ui.focus.invalid"},
                                           ErrorSeverity::Error,
                                           "The Runtime UI focus graph or transition is invalid.",
                                           "Provide one bounded rooted graph with valid stable identities and known policies.",
                                           false,
                                           false};
    /** @copydoc FocusSourceStale */
    const ErrorCodeDescriptor FocusSourceStale{UiDomain,
                                               ErrorCode{"runtime_ui.focus.source_stale"},
                                               ErrorSeverity::Error,
                                               "The Runtime UI focus source belongs to another owner or published generation.",
                                               "Rebuild focus evidence from the active canvas, tree, and last presented interaction.",
                                               true,
                                               false};
    /** @copydoc FocusTargetUnavailable */
    const ErrorCodeDescriptor
        FocusTargetUnavailable{UiDomain,
                               ErrorCode{"runtime_ui.focus.target_unavailable"},
                               ErrorSeverity::Error,
                               "The requested Runtime UI focus target is unavailable.",
                               "Resolve a resident enabled, visible, focusable element or apply the declared recovery policy.",
                               false,
                               false};
    /** @copydoc FocusModalBoundaryViolation */
    const ErrorCodeDescriptor FocusModalBoundaryViolation{UiDomain,
                                                          ErrorCode{"runtime_ui.focus.modal_boundary_violation"},
                                                          ErrorSeverity::Error,
                                                          "The Runtime UI focus transition would leave the active modal scope.",
                                                          "Keep focus within the inclusive top modal root until that modal is closed.",
                                                          false,
                                                          false};
    /** @copydoc FocusCapacityExceeded */
    const ErrorCodeDescriptor FocusCapacityExceeded{UiDomain,
                                                    ErrorCode{"runtime_ui.focus.capacity_exceeded"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI focus graph exceeds its bounded node or storage capacity.",
                                                    "Use a complete candidate within the admitted graph bound and retry at a safe point.",
                                                    true,
                                                    false};
    /** @copydoc FocusModalCapacityExceeded */
    const ErrorCodeDescriptor
        FocusModalCapacityExceeded{UiDomain,
                                   ErrorCode{"runtime_ui.focus.modal_capacity_exceeded"},
                                   ErrorSeverity::Error,
                                   "The Runtime UI focus modal or restoration stack is full.",
                                   "Close the current top modal or increase the finite scope capacity before opening another.",
                                   true,
                                   false};
    /** @copydoc FocusModalStale */
    const ErrorCodeDescriptor FocusModalStale{UiDomain,
                                              ErrorCode{"runtime_ui.focus.modal_stale"},
                                              ErrorSeverity::Error,
                                              "The Runtime UI modal close identity is stale or is not the top modal.",
                                              "Close only the exact current modal activation returned by the owning focus graph.",
                                              false,
                                              false};
    /** @copydoc FocusScopeMismatch */
    const ErrorCodeDescriptor FocusScopeMismatch{UiDomain,
                                                 ErrorCode{"runtime_ui.focus.scope_mismatch"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI focus reload changes its player, presentation layer, or owner scope.",
                                                 "Create a separate focus graph for the new audience or presentation layer.",
                                                 false,
                                                 false};
    /** @copydoc FocusLifecycleUnavailable */
    const ErrorCodeDescriptor
        FocusLifecycleUnavailable{UiDomain,
                                  ErrorCode{"runtime_ui.focus.lifecycle_unavailable"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI focus graph is retiring or stopped.",
                                  "Close focus admission before teardown and create a new graph for the next generation.",
                                  false,
                                  false};
}  // namespace Horo::Runtime::Ui::UiErrors
