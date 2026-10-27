/** @file RenderFrameGraphExecution.cpp
 * @brief Canonical frame-owned graph admission and completion-lease transfer. */
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderMultiviewResources.h"
#include "RenderResourceRegistry.h"
#include "RenderSubmissionAllocator.h"
#include "VfxSchedulingStorage.h"

#include <algorithm>
#include <memory>

namespace Horo::Render {
    namespace {
        /** @brief Resolves optional prepared transients without changing ownership or frame execution state. */
        Result<Detail::RenderGraphTransientResourceSet *> ResolveOptionalTransient(const Detail::RenderGraphTransientResourcePool &pool,
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
            if (!backend.Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing) &&
                std::ranges::any_of(workloads, [](const auto &workload) {
                return std::holds_alternative<RenderGraphVfx>(workload.workload);
            }))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxExecutionUnavailable));
            return Result<void>::Success();
        }

        /** @brief Retains the canonical VFX scratch owner only when this graph needs it and no callback already borrows it. */
        Result<std::shared_ptr<Detail::VfxSchedulingStorage>> SelectVfxResolutionStorage(
            const std::span<const RenderGraphPassWorkload> workloads, const std::shared_ptr<Detail::VfxSchedulingStorage> &storage) {
            using Selection = Result<std::shared_ptr<Detail::VfxSchedulingStorage>>;
            const auto hasVfx = std::ranges::any_of(workloads, [](const auto &entry) {
                return std::holds_alternative<RenderGraphVfx>(entry.workload);
            });
            if (!hasVfx)
                return Selection::Success(nullptr);
            if (!storage || storage->resolvingGraph)
                return Selection::Failure(MakeError(VfxFrameErrors::Pending));
            return Selection::Success(storage);
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
            return EncodeResolvedGraph(graph, workloads, ui, selectedTransient.Value(), submissions, role, submissionHistory);
        } catch (...) {  // NOSONAR(cpp:S2738) Frame ownership must recover across an arbitrary backend exception.
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::EncodeResolvedGraph */
    Result<void> RenderFrameScope::EncodeResolvedGraph(const CompiledRenderGraphExecution &graph,
                                                       const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                       Detail::RenderGraphTransientResourceSet *transient,
                                                       const std::span<const RenderGraphQueueSubmission> submissions,
                                                       const RenderGraphExecutionRole role,
                                                       const Detail::RenderSubmissionAllocator &submissionHistory) {
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
        return EncodeGraphBindings({frame_,
                                    graph,
                                    workloads,
                                    resolved.Value().View(),
                                    nullptr,
                                    transient != nullptr,
                                    nullptr,
                                    submissions,
                                    std::span{multiview.Value().meshes}.first(multiview.Value().meshCount),
                                    std::span{multiview.Value().views}.first(multiview.Value().viewCount),
                                    role,
                                    {}},
                                   ui, transient, submissionHistory);
    }

    /** @copydoc RenderFrameScope::EncodeGraphBindings */
    Result<void> RenderFrameScope::EncodeGraphBindings(RenderGraphExecutionRequest request, UiRenderSubmission *ui,
                                                       Detail::RenderGraphTransientResourceSet *transient,
                                                       const Detail::RenderSubmissionAllocator &submissionHistory) {
        const auto selectedVfx = SelectVfxResolutionStorage(request.workloads, owner_->vfxScheduling_);
        if (selectedVfx.HasError()) {
            Abort();
            return Result<void>::Failure(selectedVfx.ErrorValue());
        }
        const auto vfxStorage = selectedVfx.Value();
        const auto clearVfxResolution = [](Detail::VfxSchedulingStorage *storage) noexcept {
            storage->resolvingGraph = false;
        };
        std::unique_ptr<Detail::VfxSchedulingStorage, decltype(clearVfxResolution)> vfxResolution{vfxStorage.get(), clearVfxResolution};
        std::span<const RenderGraphMeshInstance> meshes = request.meshes;
        std::span<const RenderGraphTargetInstance> targets;
        if (vfxStorage) {
            vfxStorage->resolvingGraph = true;
            if (const auto resolvedVfx = owner_->ResolveVfxGraphResources(request.workloads, meshes, meshes, targets);
                resolvedVfx.HasError()) {
                Abort();
                return resolvedVfx;
            }
        }
        const auto leased =
            request.role == RenderGraphExecutionRole::UiComposition
                ? owner_->graphResourceLeases_->AcquireUi(request.graph.Resources(), *ui, transient, completionLease_, request.workloads)
                : owner_->graphResourceLeases_->Acquire(request.graph.Resources(), ui, transient, request.workloads,
                                                        request.graph.templateBinding_);
        if (leased.HasError()) {
            Abort();
            return Result<void>::Failure(leased.ErrorValue());
        }
        request.lease = leased.Value();
        request.meshes = meshes;
        request.targets = targets;
        return ExecuteLeasedGraph(request, submissionHistory);
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
