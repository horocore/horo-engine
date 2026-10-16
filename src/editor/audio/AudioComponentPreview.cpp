#include "Horo/Editor/AudioComponentPreview.h"

namespace Horo::Editor::AudioComponentPreviewErrors {
    namespace {
        const ErrorDomainId Domain{"horo.editor.audio_preview"};
    }

    const ErrorCodeDescriptor MissingAsset{.domain = Domain,
                                           .code = ErrorCode{"editor.audio_preview.missing_asset"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The selected sound asset is missing or unreadable.",
                                           .remediationHint = "Assign a registered supported audio clip."};
    const ErrorCodeDescriptor MissingListener{.domain = Domain,
                                              .code = ErrorCode{"editor.audio_preview.missing_listener"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Three-dimensional preview has no enabled authored listener.",
                                              .remediationHint = "Add or enable a listener in this scene."};
    const ErrorCodeDescriptor MissingBus{.domain = Domain,
                                         .code = ErrorCode{"editor.audio_preview.missing_bus"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "The authored bus is not available in the preview mixer.",
                                         .remediationHint = "Select an available bus; no implicit rerouting is performed."};
    const ErrorCodeDescriptor ProviderUnavailable{.domain = Domain,
                                                  .code = ErrorCode{"editor.audio_preview.provider_unavailable"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The selected audio source or output provider is unavailable.",
                                                  .remediationHint = "Install or select a compatible provider; no fallback is performed."};
    const ErrorCodeDescriptor Busy{.domain = Domain,
                                   .code = ErrorCode{"editor.audio_preview.busy"},
                                   .defaultSeverity = ErrorSeverity::Error,
                                   .summary = "The previous preview is still retiring.",
                                   .remediationHint = "Wait for native retirement before starting another preview."};
    const ErrorCodeDescriptor Stale{.domain = Domain,
                                    .code = ErrorCode{"editor.audio_preview.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "The preview capture no longer matches the workspace.",
                                    .remediationHint = "Refresh the current selection and source revision."};
}  // namespace Horo::Editor::AudioComponentPreviewErrors
