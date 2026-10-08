#include "Horo/Runtime/Render/PipelinePreparationErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::PipelinePreparationErrors {
    namespace {
        const ErrorDomainId Domain{"render.pipeline_preparation"};
    }

    const ErrorCodeDescriptor InvalidBudget =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.invalid_budget", ErrorSeverity::Error,
                                    "Pipeline preparation budget is invalid.",
                                    "Provide finite non-zero storage, concurrency and dispatch bounds.");
    const ErrorCodeDescriptor InvalidManifest = Detail::
        MakeErrorDescriptor(Domain, "render.pipeline_preparation.invalid_manifest", ErrorSeverity::Error,
                            "Pipeline usage manifest is invalid.",
                            "Supply unique exact pipeline keys, a unique non-zero generation and explicit cooked optional fallbacks.");
    const ErrorCodeDescriptor MissingCookedArtifact =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.missing_cooked_artifact", ErrorSeverity::Error,
                                    "Packaged pipeline usage is missing a cooked artifact.",
                                    "Cook and admit every declared target variant before release preparation.");
    const ErrorCodeDescriptor Pending =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.pending", ErrorSeverity::Warning, "Pipeline is not resident.",
                                    "Complete explicit bounded preparation outside the frame loop before activation.");
    const ErrorCodeDescriptor StaleCompletion =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.stale_completion", ErrorSeverity::Warning,
                                    "Pipeline completion is stale or mismatched.",
                                    "Discard the candidate and preserve the current resident generation.");
    const ErrorCodeDescriptor Cancelled = Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.cancelled",
                                                                      ErrorSeverity::Warning, "Pipeline preparation was cancelled.",
                                                                      "Join host-owned jobs and prepare a fresh operation generation.");
    const ErrorCodeDescriptor Closed =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.closed", ErrorSeverity::Warning, "Pipeline preparation is closed.",
                                    "Join outstanding work before retiring its resources.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.pipeline_preparation.allocation_failed", ErrorSeverity::Error,
                                    "Pipeline preparation storage allocation failed.",
                                    "Reduce the bounded manifest or dispatch envelope and retry during loading.");
}  // namespace Horo::Render::PipelinePreparationErrors
