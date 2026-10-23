/** @file RenderFrameGraphExecution.cpp
 * @brief Canonical frame-owned graph admission and completion-lease transfer. */
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderMultiviewResources.h"
#include "RenderResourceRegistry.h"
#include "RenderSubmissionAllocator.h"

#include <algorithm>
#include <memory>

namespace Horo::Render {
    namespace {
        /** @brief Resolves optional prepared transients without changing ownership or frame execution state. */
        Result<Detail::RenderGraphTransientResourceSet *> ResolveOptionalTransient(Detail::RenderGraphTransientResourcePool &pool,
                                                                                   RenderGraphTransientResourcesHandle handle,
                                                                                   const CompiledRenderGraphExecution &graph) {
            return handle.IsValid() ? pool.Resolve(handle, graph) : Result<Detail::RenderGraphTransientResourceSet *>::Success(nullptr);
        }

        /** @brief Rejects unavailable encoders before resources or native work are acquired. */
        Result<void> ValidateGraphWorkloadSupport(const IRenderBackend &backend, const std::span<const RenderGraphPassWorkload> workloads,
                                                  const bool explicitSubmissions, RenderGraphExecutionRole role,
                                                  const UiRenderSubmission *ui) {
            if (role == RenderGraphExecutionRole::UiComposition && ui == nullptr)
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            if (explicitSubmissions && !backend.Capabilities().supportsExplicitGraphSubmissions)
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                       "The selected backend does not implement explicit graph submissions."));
            if (std::ranges::any_of(workloads, [](const auto &workload) {
                return std::holds_alternative<RenderGraphVfx>(workload.workload);
            }))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxExecutionUnavailable));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderFrameScope::ExecuteGraph */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads) {
        return ExecuteGraphInternal(graph, workloads, nullptr);
    }

    /** @copydoc RenderFrameScope::ExecuteSubmittedGraph */
    Result<void> RenderFrameScope::ExecuteSubmittedGraph(const CompiledRenderGraphExecution &graph,
                                                         const std::span<const RenderGraphPassWorkload> workloads,
                                                         const std::span<const RenderGraphQueueSubmission> submissions,
                                                         const RenderGraphTransientResourcesHandle transientResources) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        if (submissions.empty()) {
            Abort();
            return Result<void>::Failure(
                MakeError(RenderGraphExecutionErrors::InvalidGraph, "Explicit graph execution requires frontend submission records."));
        }
        return ExecuteGraphInternal(graph, workloads, nullptr, transientResources, submissions);
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * UiRenderSubmission) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui) {
        return ExecuteGraphInternal(graph, workloads, &ui);
    }

    /** @copydoc RenderFrameScope::ExecuteGraphInternal */
    Result<void> RenderFrameScope::ExecuteGraphInternal(const CompiledRenderGraphExecution &graph,
                                                        const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                        const RenderGraphTransientResourcesHandle transientResources,
                                                        const std::span<const RenderGraphQueueSubmission> submissions) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        return EncodeAdmittedGraph(graph, workloads, ui, transientResources, submissions, RenderGraphExecutionRole::Scene);
    }

    /** @copydoc RenderFrameScope::EncodeAdmittedGraph */
    Result<void> RenderFrameScope::EncodeAdmittedGraph(const CompiledRenderGraphExecution &graph,
                                                       const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                       const RenderGraphTransientResourcesHandle transientResources,
                                                       const std::span<const RenderGraphQueueSubmission> submissions,
                                                       const RenderGraphExecutionRole role) {
        if (auto supported = ValidateGraphWorkloadSupport(*backend_, workloads, !submissions.empty(), role, ui); supported.HasError()) {
            Abort();
            return supported;
        }
        try {
            auto submissionHistory = *owner_->submissionAllocator_;
            if (auto imported = submissionHistory.AdmitGraphSubmissions(submissions); imported.HasError()) {
                Abort();
                return imported;
            }
            const auto selectedTransient = ResolveOptionalTransient(*owner_->graphTransientResources_, transientResources, graph);
            if (selectedTransient.HasError()) {
                Abort();
                return Result<void>::Failure(selectedTransient.ErrorValue());
            }
            auto *transient = selectedTransient.Value();
            const auto resolved = Detail::ResolveFrameGraphResources(graph, *owner_->resourceRegistry_, transient);
            if (resolved.HasError()) {
                Abort();
                return Result<void>::Failure(resolved.ErrorValue());
            }
            const auto multiview = Detail::ResolveMultiviewResources(workloads, *owner_->resourceRegistry_);
            if (multiview.HasError()) {
                Abort();
                return Result<void>::Failure(multiview.ErrorValue());
            }
            const auto leased =
                role == RenderGraphExecutionRole::UiComposition
                    ? owner_->graphResourceLeases_->AcquireUi(graph.Resources(), *ui, transient, completionLease_, workloads)
                    : owner_->graphResourceLeases_->Acquire(graph.Resources(), ui, transient, workloads);
            if (leased.HasError()) {
                Abort();
                return Result<void>::Failure(leased.ErrorValue());
            }
            return ExecuteLeasedGraph({frame_, graph, workloads, resolved.Value().View(), leased.Value(), transient != nullptr, nullptr,
                                       submissions, std::span{multiview.Value().meshes}.first(multiview.Value().meshCount),
                                       std::span{multiview.Value().views}.first(multiview.Value().viewCount), role},
                                      submissionHistory);
        } catch (...) {  // NOSONAR(cpp:S2738) Frame ownership must recover across an arbitrary backend exception.
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * RenderGraphTransientResourcesHandle) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads,
                                                const RenderGraphTransientResourcesHandle transientResources) {
        if (!transientResources.IsValid()) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        return ExecuteGraphInternal(graph, workloads, nullptr, transientResources);
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * RenderGraphTransientResourcesHandle, UiRenderSubmission) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads,
                                                const RenderGraphTransientResourcesHandle transientResources, UiRenderSubmission ui) {
        if (!transientResources.IsValid()) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        return ExecuteGraphInternal(graph, workloads, &ui, transientResources);
    }

    /** @copydoc RenderFrameScope::ExecuteLeasedGraph */
    Result<void> RenderFrameScope::ExecuteLeasedGraph(RenderGraphExecutionRequest request,
                                                      const Detail::RenderSubmissionAllocator &submissionHistory) {
        RenderGraphLeaseTransfer transfer;
        if (request.lease == completionLease_ && completionLease_ != nullptr)
            transfer.authority = RenderGraphLeaseAuthority::BackendCompletion;
        const auto releaseLease = [&transfer](IRenderGraphResourceLease *lease) {
            if (transfer.authority == RenderGraphLeaseAuthority::Frontend)
                lease->Release();
        };
        std::unique_ptr<IRenderGraphResourceLease, decltype(releaseLease)> lease{request.lease, releaseLease};
        request.transfer = &transfer;
        if (const auto result = backend_->ExecuteGraph(request); result.HasError()) {
            Abort();
            return result;
        }
        completionLease_ = lease.release();
        *owner_->submissionAllocator_ = submissionHistory;
        executed_ = true;
        return Result<void>::Success();
    }

}  // namespace Horo::Render
