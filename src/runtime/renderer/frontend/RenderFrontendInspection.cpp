#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"
#include "RenderResourceRegistry.h"

#include <limits>

namespace Horo::Render {
    /** @copydoc RenderFrameScope::CaptureInspection */
    Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> RenderFrameScope::CaptureInspection(
        const RenderGraph &graph, const RenderGraphSchedule &schedule, const RenderGraphLifetimePlan &lifetime,
        const CompiledRenderGraphExecution &execution, const RenderGraphInspectionLimits limits, const std::stop_token cancellation) {
        using SnapshotResult = Result<std::shared_ptr<const RenderGraphInspectionSnapshot>>;
        if (owner_ == nullptr || backend_ == nullptr || executed_ || !frame_.IsValid())
            return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::InvalidSource));
        const auto current = owner_->inspectionFeed_.Read();
        if (current.HasError())
            return SnapshotResult::Failure(current.ErrorValue());
        if (owner_->inspectionRevision_ == std::numeric_limits<std::uint64_t>::max())
            return SnapshotResult::Failure(MakeError(RenderGraphInspectionErrors::CapacityExceeded));
        const RenderGraphInspectionContext context{owner_->resourceRegistry_->Owner(), frame_, owner_->inspectionRevision_ + 1};
        auto capture = CaptureRenderGraphInspection(graph, schedule, lifetime, execution, context, limits, cancellation);
        if (capture.HasError())
            return capture;
        const auto published = owner_->inspectionFeed_.Publish(capture.Value());
        if (published.HasError())
            return SnapshotResult::Failure(published.ErrorValue());
        owner_->inspectionRevision_ = context.revision;
        return capture;
    }

    /** @copydoc RenderFrontend::GraphInspectionSnapshot */
    Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> RenderFrontend::GraphInspectionSnapshot() const {
        return inspectionFeed_.Read();
    }
}  // namespace Horo::Render
