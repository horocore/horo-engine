#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <utility>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::EnableDynamicResolution */
    Result<void> EditorViewportRendererVulkan::EnableDynamicResolution(const Render::DynamicResolutionPolicy &policy,
                                                                       const std::uint16_t initialPermille) {
        using namespace Render;
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::WrongThread));
        if (!initialized_ || recordedEver_ || dynamicResolution_ || resources_.Target().IsValid())
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidState));
        if (!policy.IsValid() || initialPermille < policy.minimumPermille || initialPermille > policy.maximumPermille)
            return Result<void>::Failure(MakeError(DynamicResolutionErrors::InvalidConfiguration));
        if (!color_ || !temporalGuides_ || !spatial_ || !bridge_.Port())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable));
        const auto supported = bridge_.Port()->SupportsGraphGpuDuration(device_.graphicsIdentity);
        if (supported.HasError())
            return Result<void>::Failure(supported.ErrorValue());
        if (!supported.Value())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable));
        auto &state = dynamicResolution_.emplace();
        state.policy = policy;
        state.initialScale = initialPermille;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::DynamicResolutionDiagnostics */
    Result<Render::DynamicResolutionSnapshot> EditorViewportRendererVulkan::DynamicResolutionDiagnostics() const {
        using namespace Render;
        if (owner_ != std::this_thread::get_id())
            return Result<DynamicResolutionSnapshot>::Failure(MakeError(DynamicResolutionErrors::WrongThread));
        if (!dynamicResolution_)
            return Result<DynamicResolutionSnapshot>::Failure(MakeError(DynamicResolutionErrors::InvalidState));
        return dynamicResolution_->controller.Snapshot();
    }

    /** @brief Consumes one exact completed sample in queue order at the resource boundary. */
    Result<void> EditorViewportRendererVulkan::ConsumeViewportGpuTiming(DynamicResolutionState &state) {
        if (state.count == 0)
            return Result<void>::Success();
        const auto sample = state.pending[state.head];
        const auto timing = bridge_.Port()->ObserveGraphGpuDuration(sample.signal, sample.device);
        if (timing.HasError())
            return Result<void>::Failure(timing.ErrorValue());
        if (!timing.Value().completed)
            return Result<void>::Success();
        state.head = (state.head + 1) % state.pending.size();
        --state.count;
        if (!timing.Value().elapsed)
            return Result<void>::Success();
        const auto observed = state.controller.Observe({LightingViewIdentity(), sample.id, sample.generation, *timing.Value().elapsed});
        return observed.HasError() ? Result<void>::Failure(observed.ErrorValue()) : Result<void>::Success();
    }

    /** @brief Advances the staged resource generation while returning the previous usable target when pending. */
    Result<std::optional<Render::RenderTargetHandle>> EditorViewportRendererVulkan::PrepareDynamicResources(
        const Render::RenderSceneView &scene) {
        using namespace Render;
        using Outcome = Result<std::optional<RenderTargetHandle>>;
        auto &state = *dynamicResolution_;
        if (!requestedExtent_.IsValid()) {
            state.count = 0;
            return resources_.Prepare(scene, requestedExtent_);
        }
        const FramebufferExtent output{requestedExtent_.width, requestedExtent_.height};
        if (!state.configured) {
            const auto configured = state.controller.Configure(LightingViewIdentity(), state.policy, output, state.initialScale);
            if (configured.HasError())
                return Outcome::Failure(configured.ErrorValue());
            state.configured = true;
        }
        if (const auto timing = ConsumeViewportGpuTiming(state); timing.HasError())
            return Outcome::Failure(timing.ErrorValue());
        const auto current = state.controller.Snapshot();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        const auto resize = state.controller.PreviewOutputExtent(output);
        if (resize.HasError())
            return Outcome::Failure(resize.ErrorValue());
        const auto change = output == current.Value().active.targetExtent ? current.Value().pending : std::nullopt;
        const auto desiredScene = change ? change->candidate.renderExtent : resize.Value();
        auto prepared = resources_.Prepare(scene, requestedExtent_, desiredScene);
        if (prepared.HasError()) {
            if (change) {
                if (const auto rejected = state.controller.Resolve(*change, false); rejected.HasError())
                    return Outcome::Failure(rejected.ErrorValue());
            }
            return prepared;
        }
        if (prepared.Value() && resources_.AllocatedExtent() == requestedExtent_ && resources_.SceneExtent() == desiredScene) {
            if (const auto committed = CommitDynamicExtent(state, output, desiredScene, change); committed.HasError())
                return Outcome::Failure(committed.ErrorValue());
        }
        return prepared;
    }

    /** @brief Confirms the exact published render extent and invalidates timing from its predecessor. */
    Result<void> EditorViewportRendererVulkan::CommitDynamicExtent(DynamicResolutionState &state, const Render::FramebufferExtent output,
                                                                   const Render::FramebufferExtent desiredScene,
                                                                   const std::optional<Render::DynamicResolutionChange> &change) {
        const auto current = state.controller.Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().active.targetExtent == output && current.Value().active.renderExtent == desiredScene)
            return Result<void>::Success();
        const auto result = change ? state.controller.Resolve(*change, true) : state.controller.UpdateOutputExtent(output);
        if (result.HasValue())
            state.count = 0;
        return result;
    }

    /** @brief Records timing only for ordinary viewport work using the current resource generation. */
    Result<bool> EditorViewportRendererVulkan::ArmViewportGpuTiming(const Render::FrameToken frame,
                                                                    const Render::RenderTimelinePoint signal) {
        if (!dynamicResolution_ || !dynamicResolution_->configured)
            return Result<bool>::Success(false);
        auto &state = *dynamicResolution_;
        const auto active = state.controller.Snapshot();
        if (active.HasError())
            return Result<bool>::Failure(active.ErrorValue());
        const auto armed = bridge_.Port()->ArmGraphGpuDuration(frame, signal);
        if (armed.HasValue() && armed.Value())
            state.recorded = {signal, frame.value, active.Value().active.generation, frontend_.Capabilities().support.deviceIncarnation};
        return armed;
    }

    /** @brief Queues timing only after native acceptance; an unsent attempt leaves no evidence. */
    void EditorViewportRendererVulkan::FinishViewportGpuTiming(const Render::RenderTimelinePoint signal, const bool accepted) noexcept {
        if (!dynamicResolution_)
            return;
        auto &state = *dynamicResolution_;
        const auto sample = std::exchange(state.recorded, {});
        if (!accepted || !signal.IsValid() || sample.signal != signal || state.count == state.pending.size())
            return;
        state.pending[(state.head + state.count) % state.pending.size()] = sample;
        ++state.count;
    }
}  // namespace Horo::Editor
