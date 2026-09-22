#include "Horo/Runtime/Ui/UiErrors.h"

namespace Horo::Runtime::Ui::UiErrors {
    namespace {
        const ErrorDomainId UiDomain{"horo.runtime_ui"};
    }

    /** @copydoc CookedFormatUnsupported */
    const ErrorCodeDescriptor CookedFormatUnsupported{UiDomain,
                                                      ErrorCode{"runtime_ui.cooked.format_unsupported"},
                                                      ErrorSeverity::Error,
                                                      "The Runtime UI cooked payload format is unsupported.",
                                                      "Re-cook the UI document for the active Runtime UI format.",
                                                      false,
                                                      true};
    /** @copydoc CookedPayloadMalformed */
    const ErrorCodeDescriptor CookedPayloadMalformed{UiDomain,
                                                     ErrorCode{"runtime_ui.cooked.malformed"},
                                                     ErrorSeverity::Error,
                                                     "The Runtime UI cooked payload is malformed.",
                                                     "Re-cook the asset from its validated authored document.",
                                                     false,
                                                     true};
    /** @copydoc AssetMissing */
    const ErrorCodeDescriptor AssetMissing{UiDomain,
                                           ErrorCode{"runtime_ui.asset.missing"},
                                           ErrorSeverity::Error,
                                           "A required Runtime UI asset is unavailable.",
                                           "Package or publish the declared dependency before activating the UI instance.",
                                           false,
                                           true};
    /** @copydoc AssetTypeMismatch */
    const ErrorCodeDescriptor AssetTypeMismatch{UiDomain,
                                                ErrorCode{"runtime_ui.asset.type_mismatch"},
                                                ErrorSeverity::Error,
                                                "A Runtime UI asset has an unexpected type.",
                                                "Update the dependency declaration or publish the correctly typed cooked asset.",
                                                false,
                                                true};
    /** @copydoc AssetIdentityMismatch */
    const ErrorCodeDescriptor AssetIdentityMismatch{UiDomain,
                                                    ErrorCode{"runtime_ui.asset.identity_mismatch"},
                                                    ErrorSeverity::Error,
                                                    "A cooked asset envelope names a different identity than requested.",
                                                    "Rebuild or package the artifact under its declared stable AssetId.",
                                                    false,
                                                    true};
    /** @copydoc AssetTargetMismatch */
    const ErrorCodeDescriptor AssetTargetMismatch{UiDomain,
                                                  ErrorCode{"runtime_ui.asset.target_mismatch"},
                                                  ErrorSeverity::Error,
                                                  "A cooked Runtime UI asset targets a different runtime profile.",
                                                  "Package the artifact for the exact requested cook target.",
                                                  false,
                                                  true};
    /** @copydoc AssetRegistryStale */
    const ErrorCodeDescriptor AssetRegistryStale{UiDomain,
                                                 ErrorCode{"runtime_ui.asset.registry_stale"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI asset registry changed during preparation.",
                                                 "Discard the candidate and prepare again from the latest registry snapshot.",
                                                 true,
                                                 false};
    /** @copydoc AssetPayloadEmpty */
    const ErrorCodeDescriptor AssetPayloadEmpty{UiDomain,
                                                ErrorCode{"runtime_ui.asset.payload_empty"},
                                                ErrorSeverity::Error,
                                                "A resolved Runtime UI dependency has an empty cooked payload.",
                                                "Re-cook the dependency and publish a non-empty runtime artifact.",
                                                false,
                                                true};
    /** @copydoc AssetBudgetExceeded */
    const ErrorCodeDescriptor AssetBudgetExceeded{UiDomain,
                                                  ErrorCode{"runtime_ui.asset.budget_exceeded"},
                                                  ErrorSeverity::Error,
                                                  "Runtime UI asset preparation exceeded its bounded budget.",
                                                  "Reduce dependency payloads or raise the explicit runtime UI asset limit.",
                                                  false,
                                                  true};
    /** @copydoc AssetLoadQueueFull */
    const ErrorCodeDescriptor AssetLoadQueueFull{UiDomain,
                                                 ErrorCode{"runtime_ui.asset_load.queue_full"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI asset-load request queue is full.",
                                                 "Retry after an admitted request reaches a terminal state.",
                                                 true,
                                                 false};
    /** @copydoc AssetLoadNotReady */
    const ErrorCodeDescriptor AssetLoadNotReady{UiDomain,
                                                ErrorCode{"runtime_ui.asset_load.not_ready"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI asset-load request has not reached a terminal state.",
                                                "Advance or wait for the request before consuming its result.",
                                                true,
                                                false};
    /** @copydoc AssetLoadConsumed */
    const ErrorCodeDescriptor AssetLoadConsumed{UiDomain,
                                                ErrorCode{"runtime_ui.asset_load.consumed"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI asset-load result was already consumed.",
                                                "Retain the transferred result and do not consume the handle again.",
                                                false,
                                                false};
    /** @copydoc AssetLoadShutdown */
    const ErrorCodeDescriptor AssetLoadShutdown{UiDomain,
                                                ErrorCode{"runtime_ui.asset_load.shutdown"},
                                                ErrorSeverity::Error,
                                                "The Runtime UI asset-load service is shut down.",
                                                "Submit new work through a live Runtime UI owner.",
                                                false,
                                                false};
    /** @copydoc AssetLoadCancelled */
    const ErrorCodeDescriptor AssetLoadCancelled{UiDomain,
                                                 ErrorCode{"runtime_ui.asset_load.cancelled"},
                                                 ErrorSeverity::Error,
                                                 "The Runtime UI asset-load request was cancelled.",
                                                 "Retry while the owning runtime scope remains active.",
                                                 true,
                                                 false};
}  // namespace Horo::Runtime::Ui::UiErrors
