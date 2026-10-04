#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

namespace Horo::Runtime::SceneCellPayloadErrors {
    /** @copydoc Invalid */
    const ErrorCodeDescriptor Invalid{
        .domain = ErrorDomainId{"horo.runtime_scene"},
        .code = ErrorCode{"scene.cell_payload.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene cell source, identity or storage policy is invalid.",
        .remediationHint = "Capture current complete source and owner evidence with supported schemas and sufficient limits.",
        .retryable = false,
        .userActionable = true,
    };
    /** @copydoc Stale */
    const ErrorCodeDescriptor Stale{
        .domain = ErrorDomainId{"horo.runtime_scene"},
        .code = ErrorCode{"scene.cell_payload.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene cell content or streaming attempt is no longer authoritative.",
        .remediationHint = "Capture current complete source and owner evidence with supported schemas and sufficient limits.",
        .retryable = false,
        .userActionable = true,
    };
    /** @copydoc CapacityExceeded */
    const ErrorCodeDescriptor CapacityExceeded{
        .domain = ErrorDomainId{"horo.runtime_scene"},
        .code = ErrorCode{"scene.cell_payload.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The complete scene cell baseline exceeds its captured storage policy.",
        .remediationHint = "Capture current complete source and owner evidence with supported schemas and sufficient limits.",
        .retryable = false,
        .userActionable = true,
    };
    /** @copydoc Unsupported */
    const ErrorCodeDescriptor Unsupported{
        .domain = ErrorDomainId{"horo.runtime_scene"},
        .code = ErrorCode{"scene.cell_payload.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required component mode or gameplay schema is unsupported for scene cell cooking.",
        .remediationHint = "Capture current complete source and owner evidence with supported schemas and sufficient limits.",
        .retryable = false,
        .userActionable = true,
    };
    /** @copydoc Cancelled */
    const ErrorCodeDescriptor Cancelled{
        .domain = ErrorDomainId{"horo.runtime_scene"},
        .code = ErrorCode{"scene.cell_payload.cancelled"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene cell preparation was cancelled before publication.",
        .remediationHint = "Capture current complete source and owner evidence with supported schemas and sufficient limits.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Runtime::SceneCellPayloadErrors
