#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc ImageResourceInvalid */
    const ErrorCodeDescriptor
        ImageResourceInvalid{UiDomain,
                             ErrorCode{"runtime_ui.image_resource.invalid"},
                             ErrorSeverity::Error,
                             "The Runtime UI image or atlas resource declaration is invalid.",
                             "Provide bounded pages, typed asset dependencies, normalized regions, and complete sampling metadata.",
                             false,
                             true};

    /** @copydoc ImageRegionInvalid */
    const ErrorCodeDescriptor ImageRegionInvalid{UiDomain,
                                                 ErrorCode{"runtime_ui.image_region.invalid"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI image region is invalid or absent.",
                                                 "Use a known nonzero atlas region or the standalone-image sentinel for an image resource.",
                                                 false,
                                                 true};

    /** @copydoc ImageResidencyInvalid */
    const ErrorCodeDescriptor
        ImageResidencyInvalid{UiDomain,
                              ErrorCode{"runtime_ui.image_residency.invalid"},
                              ErrorSeverity::Error,
                              "The Runtime UI image publication has incompatible residency evidence.",
                              "Publish a known state and use MissingFallback only when the resource explicitly permits fallback.",
                              false,
                              true};

    /** @copydoc ImageResourceStorageExhausted */
    const ErrorCodeDescriptor ImageResourceStorageExhausted{UiDomain,
                                                            ErrorCode{"runtime_ui.image_resource.storage_exhausted"},
                                                            ErrorSeverity::Error,
                                                            "Every bounded Runtime UI image-resource slot is occupied or retired.",
                                                            "Retire an unused resource generation or increase the admitted slot capacity "
                                                            "before publishing another resource.",
                                                            true,
                                                            false};

    /** @copydoc ImageResourceLifecycleUnavailable */
    const ErrorCodeDescriptor ImageResourceLifecycleUnavailable{UiDomain,
                                                                ErrorCode{"runtime_ui.image_resource.lifecycle_unavailable"},
                                                                ErrorSeverity::Error,
                                                                "The Runtime UI image-resource registry is closed.",
                                                                "Create a new registry for the active Runtime UI owner before publishing "
                                                                "or acquiring image resources.",
                                                                false,
                                                                false};
}  // namespace Horo::Runtime::Ui::UiErrors
