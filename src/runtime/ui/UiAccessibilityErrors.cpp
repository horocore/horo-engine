#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc AccessibilitySchemaInvalid */
    const ErrorCodeDescriptor AccessibilitySchemaInvalid{UiDomain,
                                                         ErrorCode{"runtime_ui.accessibility.schema_invalid"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI accessibility schema is invalid or unsupported.",
                                                         "Use the current closed accessibility schema and bounded typed records.",
                                                         false,
                                                         true};
    /** @copydoc AccessibilityRoleInvalid */
    const ErrorCodeDescriptor AccessibilityRoleInvalid{UiDomain,
                                                       ErrorCode{"runtime_ui.accessibility.role_invalid"},
                                                       ErrorSeverity::Error,
                                                       "The Runtime UI accessibility role is unknown or incompatible.",
                                                       "Choose a declared Horo role and provide only role-valid metadata.",
                                                       false,
                                                       true};
    /** @copydoc AccessibilityStateInvalid */
    const ErrorCodeDescriptor AccessibilityStateInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.accessibility.state_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI accessibility state is invalid for its role.",
                                                        "Use only declared state flags admitted by the semantic role.",
                                                        false,
                                                        true};
    /** @copydoc AccessibilityValueInvalid */
    const ErrorCodeDescriptor AccessibilityValueInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.accessibility.value_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI accessibility value is malformed or incompatible.",
                                                        "Provide a finite typed value supported by the semantic role.",
                                                        false,
                                                        true};
    /** @copydoc AccessibilityRangeInvalid */
    const ErrorCodeDescriptor AccessibilityRangeInvalid{UiDomain,
                                                        ErrorCode{"runtime_ui.accessibility.range_invalid"},
                                                        ErrorSeverity::Error,
                                                        "The Runtime UI accessibility range is invalid.",
                                                        "Provide finite ordered bounds, a current value inside them, and a valid step.",
                                                        false,
                                                        true};
    /** @copydoc AccessibilitySelectionInvalid */
    const ErrorCodeDescriptor AccessibilitySelectionInvalid{UiDomain,
                                                            ErrorCode{"runtime_ui.accessibility.selection_invalid"},
                                                            ErrorSeverity::Error,
                                                            "The Runtime UI accessibility selection metadata is invalid.",
                                                            "Provide a declared selection mode and an item index within its bounded count.",
                                                            false,
                                                            true};
    /** @copydoc AccessibilityTextInvalid */
    const ErrorCodeDescriptor AccessibilityTextInvalid{UiDomain,
                                                       ErrorCode{"runtime_ui.accessibility.text_invalid"},
                                                       ErrorSeverity::Error,
                                                       "The Runtime UI accessibility text is invalid or exceeds its bound.",
                                                       "Provide bounded valid UTF-8 text resolved by the Localization boundary.",
                                                       false,
                                                       true};
    /** @copydoc AccessibilityNameMissing */
    const ErrorCodeDescriptor AccessibilityNameMissing{UiDomain,
                                                       ErrorCode{"runtime_ui.accessibility.name_missing"},
                                                       ErrorSeverity::Error,
                                                       "The interactive Runtime UI accessibility node has no accessible name.",
                                                       "Provide a bounded name or a valid labelled-by relation.",
                                                       false,
                                                       true};
    /** @copydoc AccessibilityRelationInvalid */
    const ErrorCodeDescriptor
        AccessibilityRelationInvalid{UiDomain,
                                     ErrorCode{"runtime_ui.accessibility.relation_invalid"},
                                     ErrorSeverity::Error,
                                     "The Runtime UI accessibility relation is invalid.",
                                     "Reference a node in the same semantic generation exactly once and avoid cycles.",
                                     false,
                                     true};
    /** @copydoc AccessibilityActionInvalid */
    const ErrorCodeDescriptor AccessibilityActionInvalid{UiDomain,
                                                         ErrorCode{"runtime_ui.accessibility.action_invalid"},
                                                         ErrorSeverity::Error,
                                                         "The Runtime UI accessibility action is invalid.",
                                                         "Use a unique typed action admitted by the node role and state.",
                                                         false,
                                                         true};
    /** @copydoc AccessibilityContributorInvalid */
    const ErrorCodeDescriptor AccessibilityContributorInvalid{UiDomain,
                                                              ErrorCode{"runtime_ui.accessibility.contributor_invalid"},
                                                              ErrorSeverity::Error,
                                                              "The contributed Runtime UI accessibility ownership evidence is invalid.",
                                                              "Provide a non-zero contributor identity only for contributed controls.",
                                                              false,
                                                              true};
    /** @copydoc AccessibilitySnapshotInvalid */
    const ErrorCodeDescriptor AccessibilitySnapshotInvalid{UiDomain,
                                                           ErrorCode{"runtime_ui.accessibility.snapshot_invalid"},
                                                           ErrorSeverity::Error,
                                                           "The immutable Runtime UI accessibility snapshot is invalid.",
                                                           "Publish one complete bounded semantic projection with exact source evidence.",
                                                           false,
                                                           false};
    /** @copydoc AccessibilitySnapshotSourceStale */
    const ErrorCodeDescriptor
        AccessibilitySnapshotSourceStale{UiDomain,
                                         ErrorCode{"runtime_ui.accessibility.snapshot_source_stale"},
                                         ErrorSeverity::Error,
                                         "The Runtime UI accessibility snapshot source is stale or mismatched.",
                                         "Rebuild the candidate from the active tree and current interaction generation.",
                                         true,
                                         false};
    /** @copydoc AccessibilitySnapshotStorageExhausted */
    const ErrorCodeDescriptor
        AccessibilitySnapshotStorageExhausted{UiDomain,
                                              ErrorCode{"runtime_ui.accessibility_snapshot.storage_exhausted"},
                                              ErrorSeverity::Error,
                                              "Every bounded Runtime UI accessibility snapshot slot is leased.",
                                              "Retire an in-flight semantic snapshot before retrying; never allocate fallback storage.",
                                              true,
                                              false};
    /** @copydoc AccessibilityLifecycleUnavailable */
    const ErrorCodeDescriptor
        AccessibilityLifecycleUnavailable{UiDomain,
                                          ErrorCode{"runtime_ui.accessibility.lifecycle_unavailable"},
                                          ErrorSeverity::Error,
                                          "The Runtime UI accessibility snapshot store is closed.",
                                          "Create a store for the current Runtime UI owner generation before publishing.",
                                          false,
                                          false};
    /** @copydoc AccessibilityActionStale */
    const ErrorCodeDescriptor
        AccessibilityActionStale{UiDomain,
                                 ErrorCode{"runtime_ui.accessibility.action_stale"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI accessibility action targets a stale semantic generation.",
                                 "Resynchronize the semantic snapshot and submit the action against its exact revision.",
                                 true,
                                 false};
    /** @copydoc AccessibilityActionRejected */
    const ErrorCodeDescriptor
        AccessibilityActionRejected{UiDomain,
                                    ErrorCode{"runtime_ui.accessibility.action_rejected"},
                                    ErrorSeverity::Error,
                                    "The Runtime UI accessibility action is not currently admissible.",
                                    "Respect node visibility, enabled state, declared action and typed argument policy.",
                                    false,
                                    false};
    /** @copydoc AccessibilityFocusConflict */
    const ErrorCodeDescriptor
        AccessibilityFocusConflict{UiDomain,
                                   ErrorCode{"runtime_ui.accessibility.focus_conflict"},
                                   ErrorSeverity::Error,
                                   "The Runtime UI accessibility snapshot contains conflicting semantic focus.",
                                   "Publish at most one focused node for the exact audience and interaction generation.",
                                   false,
                                   true};
}  // namespace Horo::Runtime::Ui::UiErrors
