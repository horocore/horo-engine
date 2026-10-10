#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include "RenderErrorDescriptor.h"

namespace Horo::Render::RenderGraphInspectionErrors {
    namespace {
        const ErrorDomainId Domain{"render.graph.inspection"};
    }

    const ErrorCodeDescriptor InvalidSource =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.invalid_source", ErrorSeverity::Error,
                                    "Inspection sources or exact renderer/frame identities do not match.",
                                    "Capture intact compiler outputs for one graph and the exact current renderer frame.");
    const ErrorCodeDescriptor InvalidLimits =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.invalid_limits", ErrorSeverity::Error,
                                    "Inspection limits are zero or exceed hard bounds.",
                                    "Provide finite admitted record and byte allowances.");
    const ErrorCodeDescriptor CapacityExceeded =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.capacity_exceeded", ErrorSeverity::Error,
                                    "The complete inspection exceeds its record or byte allowance.",
                                    "Request a smaller graph or increase the explicit allowance within the hard bounds.");
    const ErrorCodeDescriptor Cancelled = Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.cancelled", ErrorSeverity::Warning,
                                                                      "Inspection work was cancelled before publication.",
                                                                      "Request a new explicit capture or export when needed.");
    const ErrorCodeDescriptor AllocationFailed =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.allocation_failed", ErrorSeverity::Error,
                                    "Inspection storage allocation failed.",
                                    "Release retained inspection values or reduce the requested allowance.", true);
    const ErrorCodeDescriptor WrongThread =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.wrong_thread", ErrorSeverity::Error,
                                    "Inspection publication was attempted outside the owner thread.",
                                    "Dispatch publication and reads at the host owner safe point.");
    const ErrorCodeDescriptor Closed =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.closed", ErrorSeverity::Error, "Inspection publication has shut down.",
                                    "Create a new host-owned feed after renderer initialization.");
    const ErrorCodeDescriptor StalePublication =
        Detail::MakeErrorDescriptor(Domain, "render.graph.inspection.stale_publication", ErrorSeverity::Error,
                                    "Inspection publication names an old renderer or revision.",
                                    "Publish a new capture for the current renderer generation.");
}  // namespace Horo::Render::RenderGraphInspectionErrors
