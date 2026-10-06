#include "Horo/Editor/FractureAssetDocument.h"

namespace Horo::Editor::FractureDocumentErrors {
    namespace {
        constexpr ErrorDomain Domain{"editor.fracture"};
    }

    const ErrorCodeDescriptor
        InvalidSource{.domain = Domain,
                      .code = ErrorCode{"editor.fracture.invalid_source"},
                      .defaultSeverity = ErrorSeverity::Error,
                      .summary = "Fracture authoring source violates its typed schema or graph invariants.",
                      .remediationHint = "Repair exact stable source, chunk, hierarchy, material and support references before retrying."};
    const ErrorCodeDescriptor LimitExceeded{.domain = Domain,
                                            .code = ErrorCode{"editor.fracture.limit_exceeded"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Fracture source or operation exceeds its finite authoring envelope.",
                                            .remediationHint = "Reduce the source or operation to the selected product bounds."};
    const ErrorCodeDescriptor WrongDocument{.domain = Domain,
                                            .code = ErrorCode{"editor.fracture.wrong_document"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "Fracture operation belongs to another document session.",
                                            .remediationHint = "Discard late work and capture the active asset session."};
    const ErrorCodeDescriptor StaleRevision{.domain = Domain,
                                            .code = ErrorCode{"editor.fracture.stale_revision"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "Fracture operation belongs to an older document revision.",
                                            .remediationHint = "Rebuild the typed operation against the current immutable snapshot."};
    const ErrorCodeDescriptor AuthorityDenied{.domain = Domain,
                                              .code = ErrorCode{"editor.fracture.authority_denied"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The document context has no authoring permission.",
                                              .remediationHint = "Use a host-admitted writable document capability."};
    const ErrorCodeDescriptor HistoryBudgetExceeded{.domain = Domain,
                                                    .code = ErrorCode{"editor.fracture.history_budget_exceeded"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "The complete semantic undo representation cannot be reserved.",
                                                    .remediationHint =
                                                        "Reduce the transaction or choose an explicitly admitted larger history profile."};
    const ErrorCodeDescriptor PublicationConflict{.domain = Domain,
                                                  .code = ErrorCode{"editor.fracture.publication_conflict"},
                                                  .defaultSeverity = ErrorSeverity::Warning,
                                                  .summary = "The source publication receipt no longer matches the document save fence.",
                                                  .remediationHint = "Reconcile the accepted Assets source revision before saving again."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"editor.fracture.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Warning,
                                        .summary = "Fracture authoring work was cancelled before commit.",
                                        .remediationHint = "Keep the last committed source; retry only as a new explicit operation."};
    const ErrorCodeDescriptor Closed{.domain = Domain,
                                     .code = ErrorCode{"editor.fracture.closed"},
                                     .defaultSeverity = ErrorSeverity::Warning,
                                     .summary = "The fracture document session has closed admission.",
                                     .remediationHint = "Open a fresh session; retained snapshots remain readable."};
    const ErrorCodeDescriptor Exhausted{.domain = Domain,
                                        .code = ErrorCode{"editor.fracture.exhausted"},
                                        .defaultSeverity = ErrorSeverity::Error,
                                        .summary = "The fracture document cannot issue another revision or content identity.",
                                        .remediationHint = "Save and reopen in a fresh document session; identity counters never wrap."};
    const ErrorCodeDescriptor UnsupportedVersion{.domain = Domain,
                                                 .code = ErrorCode{"editor.fracture.unsupported_version"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The fracture authoring source schema is unsupported.",
                                                 .remediationHint =
                                                     "Use an explicit source migration; do not reinterpret unknown versions."};
}  // namespace Horo::Editor::FractureDocumentErrors
