#include "Horo/UiTemplates/UiTemplateErrors.h"

namespace Horo::UiTemplates::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc TemplateGraphInvalid */
    const ErrorCodeDescriptor TemplateGraphInvalid{UiDomain,
                                                   ErrorCode{"runtime_ui.template.graph_invalid"},
                                                   ErrorSeverity::Error,
                                                   "The template dependency graph metadata is malformed or duplicated.",
                                                   "Publish a complete catalog with unique stable identities and valid revisions.",
                                                   false,
                                                   true};
    /** @copydoc TemplateMissing */
    const ErrorCodeDescriptor TemplateMissing{UiDomain,
                                              ErrorCode{"runtime_ui.template.missing"},
                                              ErrorSeverity::Error,
                                              "A required template is absent from the pinned catalog.",
                                              "Restore the exact referenced template asset before cooking.",
                                              false,
                                              true};
    /** @copydoc TemplateRevisionUnavailable */
    const ErrorCodeDescriptor TemplateRevisionUnavailable{UiDomain,
                                                          ErrorCode{"runtime_ui.template.revision_unavailable"},
                                                          ErrorSeverity::Error,
                                                          "The accepted template revision is absent from the pinned catalog.",
                                                          "Restore the accepted revision or explicitly rebase the instance.",
                                                          false,
                                                          true};
    /** @copydoc TemplateVersionIncompatible */
    const ErrorCodeDescriptor TemplateVersionIncompatible{UiDomain,
                                                          ErrorCode{"runtime_ui.template.version_incompatible"},
                                                          ErrorSeverity::Error,
                                                          "The template schema or public interface is incompatible.",
                                                          "Migrate or choose a compatible template revision.",
                                                          false,
                                                          true};
    /** @copydoc TemplateDependencyCycle */
    const ErrorCodeDescriptor TemplateDependencyCycle{UiDomain,
                                                      ErrorCode{"runtime_ui.template.dependency_cycle"},
                                                      ErrorSeverity::Error,
                                                      "Nested templates form a dependency cycle.",
                                                      "Remove the repeated template asset from the active lineage.",
                                                      false,
                                                      true};
    /** @copydoc TemplatePackageUnavailable */
    const ErrorCodeDescriptor TemplatePackageUnavailable{UiDomain,
                                                         ErrorCode{"runtime_ui.template.package_unavailable"},
                                                         ErrorSeverity::Error,
                                                         "A required package is absent or has an incompatible pinned version.",
                                                         "Restore a compatible version in the verified package lock.",
                                                         false,
                                                         true};
    /** @copydoc TemplateGraphBudgetExceeded */
    const ErrorCodeDescriptor TemplateGraphBudgetExceeded{UiDomain,
                                                          ErrorCode{"runtime_ui.template.graph_budget_exceeded"},
                                                          ErrorSeverity::Error,
                                                          "The template dependency graph exceeds its configured bounds.",
                                                          "Reduce nesting and dependency counts or raise the explicit load-time budget.",
                                                          false,
                                                          true};
    /** @copydoc TemplateGraphShutdown */
    const ErrorCodeDescriptor TemplateGraphShutdown{UiDomain,
                                                    ErrorCode{"runtime_ui.template.graph_shutdown"},
                                                    ErrorSeverity::Error,
                                                    "The template dependency resolver has stopped.",
                                                    "Create a resolver from a current pinned catalog snapshot.",
                                                    false,
                                                    false};
}  // namespace Horo::UiTemplates::UiErrors
