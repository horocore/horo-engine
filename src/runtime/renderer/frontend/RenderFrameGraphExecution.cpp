/** @file RenderFrameGraphExecution.cpp
 * @brief Canonical frame-owned graph admission and completion-lease transfer. */
#include "Horo/Runtime/Render/NativeReconstructionViewHistory.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/TemporalHistoryErrors.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderMultiviewResources.h"
#include "RenderResourceRegistry.h"
#include "RenderSubmissionAllocator.h"
#include "VfxGpuStorage.h"
#include "VfxSchedulingStorage.h"

#include <algorithm>
#include <memory>
#include <ranges>

namespace Horo::Render {
    namespace {
        /** @brief Resolves optional prepared transients without changing ownership or frame execution state. */
        Result<Detail::RenderGraphTransientResourceSet *> ResolveOptionalTransient(const Detail::RenderGraphTransientResourcePool &pool,
                                                                                   RenderGraphTransientResourcesHandle handle,
                                                                                   const CompiledRenderGraphExecution &graph) {
            return handle.IsValid() ? pool.Resolve(handle, graph) : Result<Detail::RenderGraphTransientResourceSet *>::Success(nullptr);
        }

        bool HasGpuVfx(const std::span<const RenderGraphPassWorkload> workloads) noexcept {
            return std::ranges::any_of(workloads, [](const auto &entry) {
                return std::holds_alternative<RenderGraphVfxGpuCompute>(entry.workload) ||
                       std::holds_alternative<RenderGraphVfxGpuDraw>(entry.workload);
            });
        }

        Result<void> ValidateGpuVfxSupport(const IRenderBackend &backend, const std::span<const RenderGraphPassWorkload> workloads,
                                           const bool explicitSubmissions) {
            if (!HasGpuVfx(workloads))
                return Result<void>::Success();
            if (const auto &features = backend.Capabilities().support.features; !explicitSubmissions ||
                                                                                !features.Supports(RenderCapability::VfxGpuSimulation) ||
                                                                                !features.Supports(RenderCapability::VfxGpuDraw))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxExecutionUnavailable));
            return Result<void>::Success();
        }

        Result<void> ValidateReconstructionSupport(const IRenderBackend &backend, std::span<const RenderGraphPassWorkload> workloads,
                                                   bool explicitSubmissions) {
            const auto hasReconstruction = std::ranges::any_of(workloads, [](const auto &entry) {
                return std::holds_alternative<RenderGraphNativeReconstruction>(entry.workload);
            });
            if (hasReconstruction &&
                (!explicitSubmissions || !backend.Capabilities().support.features.Supports(RenderCapability::NativeReconstruction)))
                return Result<void>::Failure(MakeError(NativeReconstructionErrors::Unsupported));
            return Result<void>::Success();
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
            if (!backend.Capabilities().supportsMaterialRasterGraph && std::ranges::any_of(workloads, [](const auto &binding) {
                return std::holds_alternative<RenderGraphMaterialRaster>(binding.workload);
            }))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload,
                                                       "The selected backend does not implement same-graph material raster consumers."));
            if (!backend.Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing) &&
                std::ranges::any_of(workloads, [](const auto &workload) {
                return std::holds_alternative<RenderGraphVfx>(workload.workload);
            }))
                return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::VfxExecutionUnavailable));
            if (const auto reconstruction = ValidateReconstructionSupport(backend, workloads, explicitSubmissions);
                reconstruction.HasError())
                return reconstruction;
            return ValidateGpuVfxSupport(backend, workloads, explicitSubmissions);
        }

        /** @brief Matches the graph-facing view and its actual resident source generation, not only the logical slot. */
        bool MatchesSceneImage(const CompiledRenderGraphExecution &graph, const RenderGraphReconstructionImage &binding,
                               const NativeReconstructionSceneImage &exact) noexcept {
            if (binding.view != exact.view)
                return false;
            if (!exact.texture.IsValid())
                return binding.resource == RenderGraphResourceId{};
            const auto resources = graph.Resources();
            if (binding.resource.owner != graph.Owner() || binding.resource.value == 0 || binding.resource.value > resources.size())
                return false;
            const auto &resource = resources[binding.resource.value - 1];
            const auto *texture = std::get_if<RenderTextureHandle>(&resource.binding);
            return resource.id == binding.resource && texture && *texture == exact.texture;
        }

        /** @brief Matches all packed shader inputs and exact image generations to a staged real frame. */
        bool MatchesScenePlan(const CompiledRenderGraphExecution &graph, const RenderGraphNativeReconstruction &workload,
                              const NativeReconstructionScenePlan &plan) noexcept {
            const auto &expected = plan.dispatch;
            const auto &actual = workload.dispatch;
            if (actual.entry != expected.entry || actual.constants.extents != expected.constants.extents ||
                actual.constants.sample != expected.constants.sample || actual.constants.depth != expected.constants.depth)
                return false;
            for (std::size_t i = 0; i < plan.images.size(); ++i)
                if (!MatchesSceneImage(graph, workload.images[i], plan.images[i]))
                    return false;
            return true;
        }

        /** @brief Requires the history lifetime and uses the exact compiled state preserved from its previous graph. */
        bool MatchesPreviousHistoryState(const CompiledRenderGraphExecution &graph, RenderGraphResourceId resource,
                                         const RenderGraphLogicalState &previous) noexcept {
            if (graph.Resources()[resource.value - 1].resourceClass != RenderGraphResourceClass::History)
                return false;
            const auto initial = std::ranges::find(graph.ImportedStates(), resource, &RenderGraphImportedState::resource);
            return initial != graph.ImportedStates().end() && initial->contents == RenderGraphInitialContents::Preserved &&
                   initial->state == previous;
        }

        /** @brief Obtains the final logical state of a retained history image, including subsequent consumers. */
        std::optional<RenderGraphLogicalState> FinalHistoryState(const CompiledRenderGraphExecution &graph,
                                                                 RenderGraphResourceId resource) noexcept {
            if (graph.Resources()[resource.value - 1].resourceClass != RenderGraphResourceClass::History)
                return std::nullopt;
            const auto transitions = graph.Transitions();
            const auto reverse = transitions | std::views::reverse;
            const auto last = std::ranges::find(reverse, resource, &RenderGraphTransition::resource);
            if (last == reverse.end())
                return std::nullopt;
            return last->newState;
        }

        /** @brief Observes the exact scheduler point covering the native compute dispatch. */
        RenderTimelinePoint ReconstructionCompletion(std::span<const RenderGraphQueueSubmission> submissions, std::size_t passIndex,
                                                     std::size_t passCount) noexcept {
            for (const auto &entry : submissions) {
                const auto range = entry.passes;
                if (range.IsValidFor(passCount) && passIndex >= range.offset && passIndex - range.offset < range.count)
                    return entry.submission.signal;
            }
            return {};
        }

        /** @brief Rejects a second producer of either staged output anywhere in the retained scene graph. */
        bool HasExclusiveHistoryWrites(const CompiledRenderGraphExecution &graph, const RenderGraphNativeReconstruction &workload,
                                       RenderGraphPassRef pass) noexcept {
            const auto textureOf = [&](RenderGraphResourceId id) {
                const auto *texture = std::get_if<RenderTextureHandle>(&graph.Resources()[id.value - 1].binding);
                return texture == nullptr ? RenderTextureHandle{} : *texture;
            };
            const auto output = [&](NativeReconstructionImage image) {
                return textureOf(workload.images[static_cast<std::size_t>(image)].resource);
            };
            const auto protectedImages = std::array{NativeReconstructionImage::OutputColor, NativeReconstructionImage::OutputDepth,
                                                    NativeReconstructionImage::PreviousColor, NativeReconstructionImage::PreviousDepth};
            std::array<RenderTextureHandle, protectedImages.size()> retained;
            std::ranges::transform(protectedImages, retained.begin(), [&](NativeReconstructionImage image) {
                return NativeReconstructionUsesImage(workload.dispatch.entry, image) ? output(image) : RenderTextureHandle{};
            });
            for (const auto &use : graph.Usages()) {
                if (use.pass == pass || use.access == RenderGraphAccess::Read)
                    continue;
                const auto texture = textureOf(use.resource);
                if (texture.IsValid() && std::ranges::find(retained, texture) != retained.end())
                    return false;
            }
            return true;
        }

        /** @brief Retains the canonical VFX scratch owner only when this graph needs it and no callback already borrows it. */
        Result<std::shared_ptr<Detail::VfxSchedulingStorage>> SelectVfxResolutionStorage(
            const std::span<const RenderGraphPassWorkload> workloads, const std::shared_ptr<Detail::VfxSchedulingStorage> &storage) {
            using Selection = Result<std::shared_ptr<Detail::VfxSchedulingStorage>>;
            if (const auto hasVfx = std::ranges::any_of(workloads,
                                                        [](const auto &entry) {
                return std::holds_alternative<RenderGraphVfx>(entry.workload);
            });
                !hasVfx)
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

    /** @copydoc RenderFrameScope::ExecuteNativeReconstructionScene */
    Result<void> RenderFrameScope::ExecuteNativeReconstructionScene(
        const CompiledRenderGraphExecution &graph, const std::span<const RenderGraphPassWorkload> workloads,
        const std::span<const RenderGraphQueueSubmission> submissions,
        const std::span<const std::shared_ptr<NativeReconstructionViewHistory>> histories,
        const RenderGraphTransientResourcesHandle transientResources) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
        };
        if (histories.empty() || histories.size() > reconstructionHistories_.size() || workloads.size() != graph.Passes().size() ||
            submissions.empty() || submissions.size() > RenderGraphSubmissionLimits::MaximumSubmissions)
            return invalid();

        using Image = NativeReconstructionImage;

        struct SceneCompletion final {
            RenderTimelinePoint point;
            NativeReconstructionHistoryStates states;
        };

        std::array<SceneCompletion, RenderGraphNativeReconstruction::MaximumPassesPerFrame> completions;
        std::size_t ownerIndex = 0;
        for (std::size_t i = 0; i < workloads.size(); ++i) {
            const auto *workload = std::get_if<RenderGraphNativeReconstruction>(&workloads[i].workload);
            if (workload == nullptr || workload->dispatch.entry == NativeReconstructionEntry::Spatial)
                continue;
            if (ownerIndex == histories.size())
                return invalid();
            const auto &history = histories[ownerIndex];
            if (!history || !history->SameFrontend(*owner_) || history->phase_ != NativeReconstructionViewHistory::Phase::Staged ||
                history->pending_->frameId != hostFrame_ || workloads[i].pass != graph.Passes()[i].pass)
                return invalid();
            if (!MatchesScenePlan(graph, *workload, history->plan_) || !HasExclusiveHistoryWrites(graph, *workload, workloads[i].pass))
                return invalid();
            const auto image = [workload](Image role) {
                return workload->images[static_cast<std::size_t>(role)].resource;
            };
            const auto &previous = history->plan_.previousStates;
            if (previous && (!MatchesPreviousHistoryState(graph, image(Image::PreviousColor), previous->color) ||
                             !MatchesPreviousHistoryState(graph, image(Image::PreviousDepth), previous->depth)))
                return invalid();
            const auto color = FinalHistoryState(graph, image(Image::OutputColor));
            const auto depth = FinalHistoryState(graph, image(Image::OutputDepth));
            const auto signal = ReconstructionCompletion(submissions, i, workloads.size());
            if (!signal.IsValid() || !color || !depth)
                return invalid();
            completions[ownerIndex++] = {signal, {*color, *depth}};
        }
        if (ownerIndex != histories.size())
            return invalid();

        const auto recorded = ExecuteSubmittedGraph(graph, workloads, submissions, transientResources);
        if (recorded.HasError()) {
            for (const auto &history : histories)
                history->Abandon();
            return recorded;
        }
        if (!owner_->graphResourceLeases_->RetainReconstructionHistories(completionLease_, histories)) {
            Abort();
            for (const auto &history : histories)
                history->Abandon();
            return invalid();
        }
        for (std::size_t i = 0; i < histories.size(); ++i) {
            reconstructionHistories_[i] = histories[i];
            histories[i]->BeginRecording(completions[i].point, completions[i].states);
        }
        reconstructionGraphTail_ = submissions.back().submission.signal;
        reconstructionHistoryCount_ = histories.size();
        return Result<void>::Success();
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

    /** @copydoc RenderFrameScope::AdmitGpuGraphDispatch */
    Result<void> RenderFrameScope::AdmitGpuGraphDispatch(const RenderGraphExecutionRequest &request,
                                                         const Detail::RenderSubmissionAllocator &history) {
        if (!HasGpuVfx(request.workloads))
            return Result<void>::Success();
        if (const auto valid = ValidateRenderGraphExecutionRequest(request); valid.HasError())
            return valid;
        if (const auto registered = owner_->RegisterVfxGpuDispatch(request, history); registered.HasError())
            return registered;
        // Admission is not acceptance, and remains recorded even when the native result is uncertain.
        *owner_->submissionAllocator_ = history;
        return Result<void>::Success();
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
        const bool gpu = HasGpuVfx(request.workloads);
        if (const auto admitted = AdmitGpuGraphDispatch(request, submissionHistory); admitted.HasError()) {
            Abort();
            return admitted;
        }
        const auto clearGpuBusy = [](Detail::VfxGpuStorage *storage) noexcept {
            storage->busy = false;
        };
        std::unique_ptr<Detail::VfxGpuStorage, decltype(clearGpuBusy)> gpuGuard{gpu ? owner_->vfxGpuStorage_.get() : nullptr, clearGpuBusy};
        if (gpuGuard)
            gpuGuard->busy = true;
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
