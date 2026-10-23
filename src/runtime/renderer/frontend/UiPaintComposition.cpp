#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"
#include "RenderFrontendErrors.h"
#include "RenderSubmissionAllocator.h"

#include <algorithm>

namespace Horo::Render {
    /** @copydoc RenderFrameScope::ValidateUiCompositionAdmission */
    Result<void> RenderFrameScope::ValidateUiCompositionAdmission() const {
        if (backend_ == nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameNotActive));
        if (const auto open = owner_->RequireOpenOwner(); open.HasError())
            return open;
        if (!executed_)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameNotExecuted));
        if (uiComposed_ || guiComposed_ || parallelWork_ || parallelGraphWork_)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameAlreadyExecuted,
                                                   "UI composition requires its unique stage after scene execution and before GUI."));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ComposeUi() */
    Result<void> RenderFrameScope::ComposeUi() {
        if (const auto admitted = ValidateUiCompositionAdmission(); admitted.HasError())
            return admitted;
        uiComposed_ = true;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ComposeUi(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * UiRenderSubmission, RenderGraphTransientResourcesHandle) */
    Result<void> RenderFrameScope::ComposeUi(const CompiledRenderGraphExecution &graph,
                                             const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui,
                                             const RenderGraphTransientResourcesHandle transientResources) {
        if (const auto admitted = ValidateUiCompositionAdmission(); admitted.HasError())
            return admitted;
        if (workloads.empty() || !std::ranges::all_of(workloads, [](const auto &binding) {
            return std::holds_alternative<RenderGraphUiPaint>(binding.workload);
        })) {
            Abort();
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                   "The named UI stage admits only prepared native UI paint operations."));
        }
        std::optional<RenderGraphQueueSubmission> submission;
        if (backend_->Capabilities().supportsExplicitGraphSubmissions) {
            std::array<RenderTimelinePoint, Detail::RenderSubmissionAllocator::MaximumQueues> waits{};
            auto reserved = ReserveSubmission(RenderQueueRole::Graphics, owner_->submissionAllocator_->AdmittedPoints(waits));
            if (reserved.HasError()) {
                Abort();
                return Result<void>::Failure(reserved.ErrorValue());
            }
            submission = {reserved.Value(), {0, graph.Passes().size()}};
        }
        const auto submissions = submission ? std::span{&*submission, 1} : std::span<const RenderGraphQueueSubmission>{};
        if (const auto encoded =
                EncodeAdmittedGraph(graph, workloads, &ui, transientResources, submissions, RenderGraphExecutionRole::UiComposition);
            encoded.HasError())
            return encoded;
        uiComposed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Render
