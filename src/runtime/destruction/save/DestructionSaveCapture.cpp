#include "DestructionSaveInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Destruction {
    namespace {
        /** @brief Return an original typed owner rejection without modifying source state. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }
    }  // namespace

    namespace Detail {
        /** @brief Require exact active chunk membership at the same paired Physics checkpoint epoch. */
        Result<void> ValidateDestructionPhysicsCapture(const CanonicalDestructionSnapshot &state,
                                                       const DestructionCanonicalArtifactView &artifact,
                                                       const DestructionSaveCaptureFence &fence,
                                                       const std::optional<DestructionPhysicsCaptureReceipt> &physics) {
            std::size_t active{};
            for (std::size_t i = 0; i < artifact.canonicalChunks.size(); ++i) {
                if ((state.masks.active[i / 8] & (std::byte{1} << (i % 8))) == std::byte{})
                    continue;
                if (!physics || active >= physics->activeChunks.size() || physics->activeChunks[active] != artifact.canonicalChunks[i])
                    return Reject<void>(DestructionErrors::StaleRevision);
                ++active;
            }
            if (!active)
                return physics ? Reject<void>(DestructionErrors::StateInvalid) : Result<void>::Success();
            if (!physics->participant.IsValid() || !physics->checkpoint.IsValid() || physics->epoch != fence.epoch ||
                physics->sceneIncarnation != fence.sceneIncarnation || physics->target != state.target ||
                active != physics->activeChunks.size())
                return Reject<void>(DestructionErrors::StaleGeneration);
            return Result<void>::Success();
        }
    }  // namespace Detail

    /** @copydoc CapturedDestructionState::CapturedDestructionState */
    CapturedDestructionState::CapturedDestructionState(CanonicalDestructionSnapshot state, DestructionSaveCaptureFence fence,
                                                       std::optional<DestructionPhysicsCaptureReceipt> physics)
        : state_(std::move(state)), fence_(std::move(fence)), physics_(std::move(physics)) {}

    /** @copydoc CapturedDestructionState::Capture */
    Result<CapturedDestructionState> CapturedDestructionState::Capture(const DestructionSaveOwnerView &source,
                                                                       CanonicalDestructionSnapshot state,
                                                                       const DestructionSaveCaptureFence &fence,
                                                                       const std::optional<DestructionPhysicsCaptureReceipt> &physics,
                                                                       const DestructionCanonicalLimits &limits) {
        const auto &owner = source.runtime;
        const auto &context = source.context;
        const auto &artifact = source.artifact;
        if (fence.cancellation.IsCancellationRequested())
            return Reject<CapturedDestructionState>(DestructionErrors::CancelledBeforeCommit);
        if (!owner.IsAdmissionOpen())
            return Reject<CapturedDestructionState>(DestructionErrors::ShutdownInProgress);
        if (auto valid = ValidateCanonicalDestructionSnapshot(state, artifact, limits); valid.HasError())
            return Result<CapturedDestructionState>::Failure(valid.ErrorValue());
        const auto &live = owner.Snapshot();
        if (!fence.sceneIncarnation || !fence.sceneRevision || !fence.epoch.IsValid() || !fence.capabilityRevision.IsValid() ||
            !fence.authority.IsValid() || !context.authority.IsValid() || context.authority.revision.Value() != fence.authority.Value() ||
            context.target != live.target || context.content != live.content ||
            context.configurationRevision != live.configurationRevision || context.capabilityRevision != fence.capabilityRevision ||
            context.simulationTick != fence.epoch.value || state.authority != fence.authority || state.target != live.target ||
            state.content != live.content || state.configuration != live.configurationRevision || state.revision != live.revision ||
            state.effectiveFeatures != live.effectiveFeatures || state.phase != live.phase)
            return Reject<CapturedDestructionState>(DestructionErrors::StaleRevision);
        if (const double quantized = std::round(static_cast<double>(live.health) * 65536.0);
            !std::isfinite(quantized) || quantized < 0 || quantized > std::numeric_limits<std::uint32_t>::max() ||
            static_cast<std::uint32_t>(quantized) != state.healthQ16)
            return Reject<CapturedDestructionState>(DestructionErrors::StateInvalid);
        if (auto paired = Detail::ValidateDestructionPhysicsCapture(state, artifact, fence, physics); paired.HasError())
            return Result<CapturedDestructionState>::Failure(paired.ErrorValue());
        if (fence.cancellation.IsCancellationRequested())
            return Reject<CapturedDestructionState>(DestructionErrors::CancelledBeforeCommit);
        return Result<CapturedDestructionState>::Success(CapturedDestructionState{std::move(state), fence, physics});
    }

    /** @copydoc CapturedDestructionState::State */
    const CanonicalDestructionSnapshot &CapturedDestructionState::State() const noexcept {
        return state_;
    }

    /** @copydoc CapturedDestructionState::Fence */
    const DestructionSaveCaptureFence &CapturedDestructionState::Fence() const noexcept {
        return fence_;
    }

    /** @copydoc CapturedDestructionState::Physics */
    const std::optional<DestructionPhysicsCaptureReceipt> &CapturedDestructionState::Physics() const noexcept {
        return physics_;
    }
}  // namespace Horo::Destruction
