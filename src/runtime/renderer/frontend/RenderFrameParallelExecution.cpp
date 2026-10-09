#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderParallelWorkErrors.h"
#include "RenderParallelWorkInternal.h"
#include "RenderResourceRegistry.h"

#include <array>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Checks the native handoff envelope before resolving resources or allocating jobs. */
        [[nodiscard]] Result<void> ValidateParallelGraphAdmission(const RenderParallelRecordingCapabilities &capability,
                                                                  const CompiledRenderGraphExecution &graph,
                                                                  const std::span<const RenderGraphPassWorkload> workloads,
                                                                  const CancellationToken &cancellation) {
            if (capability.maximumPasses == 0 || capability.maximumLiveRecordings == 0)
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
            if (graph.Passes().size() > capability.maximumPasses)
                return Result<void>::Failure(MakeError(Detail::ParallelWorkErrors::CapacityExceeded));
            if (!graph.Owner().IsValid() || graph.Resources().size() > RenderGraphLimits::HardMaxResources ||
                workloads.size() != graph.Passes().size())
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Detail::ParallelWorkErrors::Cancelled));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderFrameScope::ValidateExecutionAdmission */
    Result<void> RenderFrameScope::ValidateExecutionAdmission() const {
        if (backend_ == nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::FrameNotActive, "Renderer frame scope no longer owns a frame."));
        if (executed_ || parallelWork_ || parallelGraphWork_)
            return Result<void>::Failure(
                MakeError(FrontendErrors::FrameAlreadyExecuted, "Renderer frame scope has already executed its pass sequence."));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ValidateStaticMeshBinding */
    Result<void> RenderFrameScope::ValidateStaticMeshBinding(const RenderPassDescriptor &pass) const {
        if (pass.primaryOutput && pass.staticMesh)
            return Result<void>::Failure(MakeError(FrontendErrors::AmbiguousPassWorkload,
                                                   "A render pass cannot bind primary-output and static-mesh workloads together."));
        if (!pass.staticMesh)
            return Result<void>::Success();
        if (owner_->staticMeshPassExecutor_ == nullptr)
            return Result<void>::Failure(
                MakeError(FrontendErrors::StaticMeshExecutorMissing, "Static-mesh pass requires an attached backend executor."));
        if (!pass.staticMesh->IsValid())
            return Result<void>::Failure(MakeError(FrontendErrors::InvalidStaticMeshPass, "Static-mesh pass descriptor is invalid."));
        if (!owner_->IsLiveTarget(pass.staticMesh->target, pass.staticMesh->extent))
            return Result<void>::Failure(
                MakeError(FrontendErrors::StaleRenderTarget, "Static-mesh pass references a stale target or mismatched target extent."));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::PrepareParallelExecution */
    Result<void> RenderFrameScope::PrepareParallelExecution(JobSystem &jobs, const std::span<const RenderPassDescriptor> orderedPasses,
                                                            const RenderParallelWorkLimits &limits, const CancellationToken &cancellation) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        // Restricts admission even when the host configured Block queues; it cannot widen a
        // surrounding producer role or turn the render owner into a scheduler-waiting thread.
        const JobProducerScope producer{JobProducerRole::RenderOwner};
        auto started = Detail::RenderParallelWorkState::Start(jobs, frame_, orderedPasses, limits, cancellation);
        if (started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        parallelWork_ = std::move(started).Value();
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::PrepareParallelGraphExecution */
    Result<void> RenderFrameScope::PrepareParallelGraphExecution(JobSystem &jobs, const CompiledRenderGraphExecution &graph,
                                                                 const std::span<const RenderGraphPassWorkload> workloads,
                                                                 const CancellationToken &cancellation) noexcept {
        try {
            if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
                return admitted;
            if (const auto admitted =
                    ValidateParallelGraphAdmission(backend_->ParallelRecordingCapabilities(), graph, workloads, cancellation);
                admitted.HasError())
                return admitted;
            auto prepared = CaptureParallelGraph(graph, workloads);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (const auto &recording = prepared.Value();
                !recording || recording->Frame() != frame_ || recording->PassCount() != workloads.size()) {
                Abort();
                return Result<void>::Failure(MakeError(FrontendErrors::InvalidFrameToken));
            }
            const JobProducerScope producer{JobProducerRole::RenderOwner};
            auto started = Detail::RenderParallelGraphWorkState::Start(jobs, std::move(prepared).Value(), cancellation);
            if (started.HasError()) {
                Abort();
                return Result<void>::Failure(started.ErrorValue());
            }
            parallelGraphWork_ = std::move(started).Value();
            return Result<void>::Success();
        } catch (...) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::CaptureParallelGraph */
    Result<std::shared_ptr<IRenderParallelGraphRecording>> RenderFrameScope::CaptureParallelGraph(
        const CompiledRenderGraphExecution &graph, const std::span<const RenderGraphPassWorkload> workloads) {
        using CaptureResult = Result<std::shared_ptr<IRenderParallelGraphRecording>>;
        const auto resolved = Detail::ResolveFrameGraphResources(graph, *owner_->resourceRegistry_);
        if (resolved.HasError())
            return CaptureResult::Failure(resolved.ErrorValue());
        const auto leased = owner_->graphResourceLeases_->Acquire(graph.Resources());
        if (leased.HasError())
            return CaptureResult::Failure(leased.ErrorValue());
        const auto releaseLease = [](IRenderGraphResourceLease *lease) {
            lease->Release();
        };
        std::unique_ptr<IRenderGraphResourceLease, decltype(releaseLease)> lease{leased.Value(), releaseLease};
        auto prepared = backend_->PrepareParallelGraph({frame_, graph, workloads, resolved.Value().View(), lease.get()});
        if (prepared.HasValue())
            static_cast<void>(lease.release());
        return prepared;
    }

    /** @copydoc RenderFrameScope::PollParallelExecution */
    Result<RenderParallelExecutionProgress> RenderFrameScope::PollParallelExecution() {
        using PollResult = Result<RenderParallelExecutionProgress>;
        if (backend_ != nullptr && parallelGraphWork_)
            return PollParallelGraph();
        if (backend_ == nullptr || !parallelWork_)
            return PollResult::Failure(MakeError(FrontendErrors::FrameNotActive, "No parallel frame execution is active."));
        if (parallelWork_->Frame() != frame_)
            return RejectParallelPoll(MakeError(FrontendErrors::InvalidFrameToken, "Parallel work belongs to a stale frame."));
        const auto progress = parallelWork_->Poll();
        if (progress.HasError())
            return RejectParallelPoll(progress.ErrorValue());
        if (progress.Value() == Detail::RenderWorkProgress::Pending)
            return PollResult::Success(RenderParallelExecutionProgress::Pending);
        // Keep captured storage alive throughout the synchronous backend/executor borrow.
        // Removing the pending controller permits the ordinary exactly-once Execute path.
        const auto completed = std::move(parallelWork_);
        if (const Result<void> executed = ExecuteCapturedCommands(completed->Commands()); executed.HasError())
            return PollResult::Failure(executed.ErrorValue());
        return PollResult::Success(RenderParallelExecutionProgress::Executed);
    }

    /** @copydoc RenderFrameScope::RejectParallelPoll */
    Result<RenderParallelExecutionProgress> RenderFrameScope::RejectParallelPoll(const Error &error) {
        // Copy first: Abort destroys the controller that may own the borrowed error.
        const Error retained = error;
        Abort();
        return Result<RenderParallelExecutionProgress>::Failure(retained);
    }

    /** @copydoc RenderFrameScope::PollParallelGraph */
    Result<RenderParallelExecutionProgress> RenderFrameScope::PollParallelGraph() noexcept {
        using PollResult = Result<RenderParallelExecutionProgress>;
        try {
            const auto &recording = parallelGraphWork_->Recording();
            if (recording->Frame() != frame_)
                return RejectParallelPoll(MakeError(FrontendErrors::InvalidFrameToken));
            const auto progress = parallelGraphWork_->Poll();
            if (progress.HasError())
                return RejectParallelPoll(progress.ErrorValue());
            if (progress.Value() == Detail::RenderWorkProgress::Pending)
                return PollResult::Success(RenderParallelExecutionProgress::Pending);
            if (const auto accepted = backend_->AcceptParallelGraph(recording); accepted.HasError())
                return RejectParallelPoll(accepted.ErrorValue());
            parallelGraphWork_.reset();
            executed_ = true;
            return PollResult::Success(RenderParallelExecutionProgress::Executed);
        } catch (...) {
            return RejectParallelPoll(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::ExecuteCapturedCommands */
    Result<void> RenderFrameScope::ExecuteCapturedCommands(const std::span<const RenderPassDescriptor> commands) noexcept {
        try {
            for (const auto &pass : commands) {
                if (const auto valid = ValidateStaticMeshBinding(pass); valid.HasError()) {
                    Abort();
                    return valid;
                }
            }
            for (const auto &pass : commands) {
                if (pass.staticMesh) {
                    const auto executed = owner_->staticMeshPassExecutor_->ExecuteStaticMeshPass(*pass.staticMesh);
                    if (executed.HasError()) {
                        Abort();
                        return executed;
                    }
                }
                const auto encoded = backend_->Execute({frame_, std::span{&pass, std::size_t{1}}});
                if (encoded.HasError()) {
                    Abort();
                    return encoded;
                }
            }
            if (commands.empty()) {
                const auto encoded = backend_->Execute({frame_, {}});
                if (encoded.HasError()) {
                    Abort();
                    return encoded;
                }
            }
            executed_ = true;
            return Result<void>::Success();
        } catch (...) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

}  // namespace Horo::Render
