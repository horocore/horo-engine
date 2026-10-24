#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
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
        if (uiComposed_ || uiComposing_ || guiComposed_ || parallelWork_ || parallelGraphWork_)
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

    /** @brief Bounded same-owner batch handoff; trial reservations and borrowed wait arrays stay alive through native encoding. */
    struct RenderFrameScope::UiCompositionState final {
        IRenderGraphResourceLease *lease{};
        bool backendOwned{};
        bool transferred{};
        bool explicitSubmissions{};
        Detail::RenderSubmissionAllocator trial;
        std::array<RenderGraphQueueSubmission, 64> submissions;
        std::array<Detail::RenderGraphTransientResourceSet *, 64> transients{};

        ~UiCompositionState() {
            if (lease && !backendOwned && !transferred)
                lease->Release();
        }
    };

    /** @copydoc RenderFrameScope::ComposeUi(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * UiRenderSubmission, RenderGraphTransientResourcesHandle) */
    Result<void> RenderFrameScope::ComposeUi(const CompiledRenderGraphExecution &graph,
                                             const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui,
                                             const RenderGraphTransientResourcesHandle transientResources) {
        UiPaintGraphComposition contribution{&graph, workloads, std::move(ui), transientResources};
        return ComposeUi(std::span{&contribution, 1});
    }

    namespace {
        /** @brief Validates the complete bounded ordered workload inventory before retaining sources or encoding any prefix. */
        Result<void> ValidateUiCompositionInventory(const std::span<UiPaintGraphComposition> canvases) {
            if (canvases.empty() || canvases.size() > 64)
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
            std::size_t passes{}, resources{}, draws{};
            for (auto &canvas : canvases) {
                if (!canvas.graph || canvas.workloads.empty() || canvas.graph->Passes().size() != canvas.workloads.size())
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
                const auto add = [](std::size_t &sum, const std::size_t value, const std::size_t limit) {
                    if (value > limit || sum > limit - value)
                        return false;
                    sum += value;
                    return true;
                };
                if (!add(passes, canvas.workloads.size(), RenderGraphLimits::HardMaxPasses) ||
                    !add(resources, canvas.graph->Resources().size(), RenderGraphLimits::HardMaxResources))
                    return Result<void>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
                for (const auto &workload : canvas.workloads) {
                    const auto *paint = std::get_if<RenderGraphUiPaint>(&workload.workload);
                    if (!paint || !add(draws, paint->draws.size(), UiPaintLimits::MaximumDraws))
                        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RenderFrameScope::PrepareUiComposition */
    Result<void> RenderFrameScope::PrepareUiComposition(const std::span<UiPaintGraphComposition> canvases, UiCompositionState &state) {
        if (auto valid = ValidateUiCompositionInventory(canvases); valid.HasError())
            return valid;
        state.lease = completionLease_;
        state.backendOwned = completionLease_ != nullptr;
        state.trial = *owner_->submissionAllocator_;
        state.explicitSubmissions = backend_->Capabilities().supportsExplicitGraphSubmissions;
        for (std::size_t index = 0; index < canvases.size(); ++index)
            if (auto prepared = PrepareUiCompositionEntry(canvases[index], state, index); prepared.HasError())
                return prepared;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::PrepareUiCompositionEntry */
    Result<void> RenderFrameScope::PrepareUiCompositionEntry(UiPaintGraphComposition &canvas, UiCompositionState &state,
                                                             const std::size_t index) {
        if (canvas.transient.IsValid()) {
            const auto selected = owner_->graphTransientResources_->Resolve(canvas.transient, *canvas.graph);
            if (selected.HasError())
                return Result<void>::Failure(selected.ErrorValue());
            state.transients[index] = selected.Value();
        }
        auto resolved = Detail::ResolveFrameGraphResources(*canvas.graph, *owner_->resourceRegistry_, state.transients[index]);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        if (state.explicitSubmissions) {
            std::array<RenderTimelinePoint, Detail::RenderSubmissionAllocator::MaximumQueues> waits{};
            auto reserved = state.trial.Reserve(RenderQueueRole::Graphics, state.trial.AdmittedPoints(waits));
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            state.submissions[index] = {reserved.Value(), {0, canvas.graph->Passes().size()}};
            if (auto admitted = state.trial.AdmitGraphSubmissions(std::span{&state.submissions[index], 1}); admitted.HasError())
                return admitted;
        }
        const auto retained = owner_->graphResourceLeases_->AcquireUi(canvas.graph->Resources(), canvas.source, state.transients[index],
                                                                      state.lease, canvas.workloads);
        if (retained.HasError())
            return Result<void>::Failure(retained.ErrorValue());
        state.lease = retained.Value();
        const auto submissions =
            state.explicitSubmissions ? std::span{&state.submissions[index], 1} : std::span<const RenderGraphQueueSubmission>{};
        const auto valid = ValidateRenderGraphExecutionRequest({frame_,
                                                                *canvas.graph,
                                                                canvas.workloads,
                                                                resolved.Value().View(),
                                                                state.lease,
                                                                state.transients[index] != nullptr,
                                                                nullptr,
                                                                submissions,
                                                                {},
                                                                {},
                                                                RenderGraphExecutionRole::UiComposition});
        if (valid.HasError())
            return valid;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::EncodeUiComposition */
    Result<void> RenderFrameScope::EncodeUiComposition(const std::span<UiPaintGraphComposition> canvases, UiCompositionState &state) {
        auto history = *owner_->submissionAllocator_;
        for (std::size_t index = 0; index < canvases.size(); ++index) {
            auto &canvas = canvases[index];
            auto resolved = Detail::ResolveFrameGraphResources(*canvas.graph, *owner_->resourceRegistry_, state.transients[index]);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            const auto submissions =
                state.explicitSubmissions ? std::span{&state.submissions[index], 1} : std::span<const RenderGraphQueueSubmission>{};
            if (auto admitted = history.AdmitGraphSubmissions(submissions); admitted.HasError())
                return admitted;
            // ExecuteLeasedGraph now owns first-transfer cleanup; subsequent entries extend that SAME backend completion lease.
            state.transferred = true;
            const auto encoded = ExecuteLeasedGraph({frame_,
                                                     *canvas.graph,
                                                     canvas.workloads,
                                                     resolved.Value().View(),
                                                     state.lease,
                                                     state.transients[index] != nullptr,
                                                     nullptr,
                                                     submissions,
                                                     {},
                                                     {},
                                                     RenderGraphExecutionRole::UiComposition},
                                                    history);
            if (encoded.HasError())
                return encoded;
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrameScope::ComposeUi(std::span<UiPaintGraphComposition>) */
    Result<void> RenderFrameScope::ComposeUi(const std::span<UiPaintGraphComposition> canvases) {
        if (auto admitted = ValidateUiCompositionAdmission(); admitted.HasError())
            return admitted;

        struct StageFence final {
            bool &active;

            explicit StageFence(bool &flag) : active(flag) {
                active = true;
            }

            ~StageFence() {
                active = false;
            }
        } fence{uiComposing_};

        try {
            UiCompositionState state;
            if (auto prepared = PrepareUiComposition(canvases, state); prepared.HasError()) {
                Abort();
                return prepared;
            }
            if (auto encoded = EncodeUiComposition(canvases, state); encoded.HasError()) {
                Abort();
                return encoded;
            }
            uiComposed_ = true;
            return Result<void>::Success();
        } catch (...) {  // NOSONAR(cpp:S2738) Canonical frame abort must preserve native completion debt across backend exceptions.
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

}  // namespace Horo::Render
