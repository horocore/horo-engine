#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }
}  // namespace Horo::Runtime::Ui::UiErrors

namespace Horo::Runtime::Ui::UiErrors {
    /** @copydoc TextLayoutInputInvalid */
    const ErrorCodeDescriptor
        TextLayoutInputInvalid{UiDomain,
                               ErrorCode{"runtime_ui.text_layout.input_invalid"},
                               ErrorSeverity::Error,
                               "The Runtime UI text layout input or result is invalid.",
                               "Provide complete shaped cluster evidence, closed text policies, and finite logical bounds.",
                               false,
                               true};
    /** @copydoc TextLayoutSourceStale */
    const ErrorCodeDescriptor TextLayoutSourceStale{UiDomain,
                                                    ErrorCode{"runtime_ui.text_layout.source_stale"},
                                                    ErrorSeverity::Error,
                                                    "The Runtime UI text layout source belongs to another or older generation.",
                                                    "Prepare text from the active element, tree, content, intrinsic, and policy revisions.",
                                                    true,
                                                    false};
    /** @copydoc TextLayoutCapacityExceeded */
    const ErrorCodeDescriptor
        TextLayoutCapacityExceeded{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.capacity_exceeded"},
                                   ErrorSeverity::Error,
                                   "The Runtime UI text layout candidate exceeds its fixed capacity.",
                                   "Reduce source text, line, glyph, or face-run count within the declared owner limits.",
                                   true,
                                   false};
    /** @copydoc TextLayoutEllipsisInvalid */
    const ErrorCodeDescriptor
        TextLayoutEllipsisInvalid{UiDomain,
                                  ErrorCode{"runtime_ui.text_layout.ellipsis_invalid"},
                                  ErrorSeverity::Error,
                                  "The Runtime UI text layout needs a valid pre-shaped ellipsis view.",
                                  "Shape the declared ellipsis with the same font and content generation before layout.",
                                  false,
                                  true};
    /** @copydoc TextLayoutStorageExhausted */
    const ErrorCodeDescriptor
        TextLayoutStorageExhausted{UiDomain,
                                   ErrorCode{"runtime_ui.text_layout.storage_exhausted"},
                                   ErrorSeverity::Error,
                                   "Every bounded Runtime UI text layout result slot is still leased.",
                                   "Retire an in-flight text layout result before retrying; never overwrite or allocate fallback storage.",
                                   true,
                                   false};
    /** @copydoc TextLayoutLifecycleUnavailable */
    const ErrorCodeDescriptor
        TextLayoutLifecycleUnavailable{UiDomain,
                                       ErrorCode{"runtime_ui.text_layout.lifecycle_unavailable"},
                                       ErrorSeverity::Error,
                                       "The Runtime UI text layout engine is closed.",
                                       "Create a new text layout engine for the active element generation before submitting work.",
                                       false,
                                       false};
}  // namespace Horo::Runtime::Ui::UiErrors
