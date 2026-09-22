#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc TextInputInvalid */
    const ErrorCodeDescriptor TextInputInvalid{UiDomain,
                                               ErrorCode{"runtime_ui.text.input.invalid"},
                                               ErrorSeverity::Error,
                                               "The Runtime UI text shaping input is malformed.",
                                               "Provide valid UTF-8, source revisions, script, language, direction, and size evidence "
                                               "within the reserved bounds.",
                                               false,
                                               true};
    /** @copydoc TextFeatureInvalid */
    const ErrorCodeDescriptor TextFeatureInvalid{UiDomain,
                                                 ErrorCode{"runtime_ui.text.feature.invalid"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI text feature request is malformed.",
                                                 "Use a four-byte OpenType feature tag and a bounded non-inverted source range.",
                                                 false,
                                                 true};
    /** @copydoc TextFontInvalid */
    const ErrorCodeDescriptor TextFontInvalid{UiDomain,
                                              ErrorCode{"runtime_ui.text.font.invalid"},
                                              ErrorSeverity::Error,
                                              "The Runtime UI font face payload is invalid.",
                                              "Provide a bounded validated cooked font face and a non-zero stable face identity.",
                                              false,
                                              true};
    /** @copydoc TextFallbackInvalid */
    const ErrorCodeDescriptor
        TextFallbackInvalid{UiDomain,
                            ErrorCode{"runtime_ui.text.fallback.invalid"},
                            ErrorSeverity::Error,
                            "The Runtime UI font fallback chain is invalid.",
                            "Provide a non-empty ordered chain of valid unique faces with an explicit missing-glyph policy.",
                            false,
                            true};
    /** @copydoc TextMissingCoverage */
    const ErrorCodeDescriptor
        TextMissingCoverage{UiDomain,
                            ErrorCode{"runtime_ui.text.missing_coverage"},
                            ErrorSeverity::Error,
                            "The Runtime UI text contains a cluster with no covering fallback face.",
                            "Add a declared face covering the complete cluster or select a non-strict missing-glyph policy.",
                            false,
                            true};
    /** @copydoc TextShapeInvalid */
    const ErrorCodeDescriptor TextShapeInvalid{UiDomain,
                                               ErrorCode{"runtime_ui.text.shape.invalid"},
                                               ErrorSeverity::Error,
                                               "The Unicode shaping backend returned invalid bounded evidence.",
                                               "Retain the last-good text result and replace the incompatible font or shaping candidate.",
                                               true,
                                               false};
    /** @copydoc TextShapeStorageExhausted */
    const ErrorCodeDescriptor TextShapeStorageExhausted{UiDomain,
                                                        ErrorCode{"runtime_ui.text_shape.storage_exhausted"},
                                                        ErrorSeverity::Error,
                                                        "Every preallocated Runtime UI text-shape slot is leased.",
                                                        "Retain fewer shape results or retry after an older result lease retires.",
                                                        true,
                                                        false};
    /** @copydoc TextLifecycleUnavailable */
    const ErrorCodeDescriptor
        TextLifecycleUnavailable{UiDomain,
                                 ErrorCode{"runtime_ui.text.lifecycle_unavailable"},
                                 ErrorSeverity::Error,
                                 "The Runtime UI text shaper is closed.",
                                 "Stop submitting text work during retirement and create a new owner-generation shaper after activation.",
                                 false,
                                 false};
}  // namespace Horo::Runtime::Ui::UiErrors
