#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsSamePlayerStableIdentity(const SequencePlayerHandle &left, const SequencePlayerHandle &right) noexcept {
            return left.session.stableValue == right.session.stableValue && left.player.stableValue == right.player.stableValue;
        }

        [[nodiscard]] bool IsTerminal(const SequencePlaybackState state) noexcept {
            return state == SequencePlaybackState::Stopped || state == SequencePlaybackState::Failed;
        }
    }  // namespace

    CinematicRuntimeService::Instance::Instance(SequencePlayer playerValue, const SequenceFrameCursor &cursorValue,
                                                SequencePlaybackActivation activationValue, std::vector<float> baselineValues,
                                                std::optional<SequenceCoordinationLease> gameplayPauseLeaseValue,
                                                std::optional<SequenceCoordinationLease> hudSuppressionLeaseValue) noexcept
        : player(std::move(playerValue)), plan(std::move(activationValue.plan)), cursor(cursorValue), blend(activationValue.blend),
          authority(std::move(activationValue.authority)), restore(std::move(activationValue.restore)),
          blendBaselines(std::move(baselineValues)), coordination(activationValue.coordination),
          coordinationHooks(activationValue.coordinationHooks), gameplayPauseLease(std::move(gameplayPauseLeaseValue)),
          hudSuppressionLease(std::move(hudSuppressionLeaseValue)),
          retainedBytes(activationValue.retainedBytes + blendBaselines.size() * sizeof(float)) {}

    CinematicRuntimeService::CinematicRuntimeService(const CinematicRuntimeSessionId session, const SequenceEvaluationBudget &budget,
                                                     std::vector<Slot> slots) noexcept
        : session_(session), budget_(budget), slots_(std::move(slots)) {}

    CinematicRuntimeService::CinematicRuntimeService(CinematicRuntimeService &&other) noexcept
        : session_(other.session_), budget_(other.budget_), usage_(other.usage_), admissionOpen_(other.admissionOpen_),
          slots_(std::move(other.slots_)) {
        other.usage_ = {};
        other.admissionOpen_ = false;
    }

    CinematicRuntimeService &CinematicRuntimeService::operator=(CinematicRuntimeService &&other) noexcept {
        if (this == &other)
            return *this;
        for (Slot &slot : slots_) {
            if (slot.instance.has_value())
                ReleaseCoordination(*slot.instance);
        }
        session_ = other.session_;
        budget_ = other.budget_;
        usage_ = other.usage_;
        admissionOpen_ = other.admissionOpen_;
        slots_ = std::move(other.slots_);
        other.usage_ = {};
        other.admissionOpen_ = false;
        return *this;
    }

    CinematicRuntimeService::~CinematicRuntimeService() noexcept {
        for (Slot &slot : slots_) {
            if (slot.instance.has_value())
                ReleaseCoordination(*slot.instance);
        }
    }

    /** @copydoc CinematicRuntimeService::Create */
    Result<CinematicRuntimeService> CinematicRuntimeService::Create(const CinematicRuntimeServiceConfig &config) {
        if (!config.session.IsValid())
            return Failed<CinematicRuntimeService>(SequencePlaybackRuntimeErrors::SessionInvalid);
        auto budget = GetSequenceEvaluationBudget(config.tier);
        if (budget.HasError())
            return Result<CinematicRuntimeService>::Failure(budget.ErrorValue());
        std::vector<Slot> slots;
        slots.resize(budget.Value().maximumActivePlayers);
        return Result<CinematicRuntimeService>::Success(CinematicRuntimeService{config.session, budget.Value(), std::move(slots)});
    }

    /** @copydoc CinematicRuntimeService::Activate */
    Result<SequencePlayerHandle> CinematicRuntimeService::Activate(SequencePlaybackActivation activation) {
        if (!admissionOpen_)
            return Failed<SequencePlayerHandle>(SequencePlaybackRuntimeErrors::AdmissionClosed);
        if (auto valid = ValidateActivation(activation); valid.HasError())
            return Result<SequencePlayerHandle>::Failure(valid.ErrorValue());
        for (const Slot &slot : slots_) {
            if (slot.instance.has_value() && IsSamePlayerStableIdentity(slot.instance->player.Snapshot().handle, activation.player.handle))
                return Failed<SequencePlayerHandle>(SequencePlaybackRuntimeErrors::DuplicateHandle);
            if (slot.hasRetiredHandle && IsSamePlayerStableIdentity(slot.retiredHandle, activation.player.handle) &&
                activation.player.handle.player.generation <= slot.retiredHandle.player.generation)
                return Failed<SequencePlayerHandle>(SequencePlaybackRuntimeErrors::HandleStale);
        }
        auto player = SequencePlayer::Create(activation.player);
        if (player.HasError())
            return Result<SequencePlayerHandle>::Failure(player.ErrorValue());
        auto cursor = MakeSequenceFrameCursor(player.Value().Snapshot(), SequenceCursorResetPolicy::EmitCurrentBoundary);
        if (cursor.HasError())
            return Result<SequencePlayerHandle>::Failure(cursor.ErrorValue());

        SequenceEvaluationUsage additional{};
        std::size_t slotIndex{};
        if (auto admitted = AdmitActivation(activation, additional, slotIndex); admitted.HasError())
            return Result<SequencePlayerHandle>::Failure(admitted.ErrorValue());

        std::vector<float> baselines;
        if (activation.blend.blendIn.mode == SequenceBlendMode::Blend || activation.blend.blendOut.mode == SequenceBlendMode::Blend) {
            baselines.reserve(activation.plan.TrackCount());
            for (const SequenceFrameTrackDescriptor &track : activation.plan.Tracks()) {
                const auto found = std::ranges::find_if(activation.restore->Entries(), [&](const SequenceRestoreEntry &entry) {
                    return entry.track == track.track;
                });
                baselines.push_back(found->value);  // Activation validation proved unique coverage.
            }
        }

        std::optional<SequenceCoordinationLease> gameplayPauseLease;
        std::optional<SequenceCoordinationLease> hudSuppressionLease;
        if (auto acquired = AcquireCoordinationLeases(activation, gameplayPauseLease, hudSuppressionLease); acquired.HasError())
            return Result<SequencePlayerHandle>::Failure(acquired.ErrorValue());

        const SequencePlayerHandle handle = activation.player.handle;
        auto playerValue = std::move(player).Value();
        slots_[slotIndex].instance.emplace(std::move(playerValue), std::move(cursor).Value(), std::move(activation), std::move(baselines),
                                           std::move(gameplayPauseLease), std::move(hudSuppressionLease));
        usage_.activePlayers += additional.activePlayers;
        usage_.aggregateTracks += additional.aggregateTracks;
        usage_.boundaryOccurrences += additional.boundaryOccurrences;
        usage_.retainedBytes += additional.retainedBytes;
        usage_.maximumLoopCrossings = std::max(usage_.maximumLoopCrossings, additional.maximumLoopCrossings);
        return Result<SequencePlayerHandle>::Success(handle);
    }

    /** @copydoc CinematicRuntimeService::Snapshot */
    Result<SequencePlayerSnapshot> CinematicRuntimeService::Snapshot(const SequencePlayerHandle &handle) const {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerSnapshot>::Failure(slot.ErrorValue());
        return Result<SequencePlayerSnapshot>::Success(slots_[slot.Value()].instance->player.Snapshot());
    }

    /** @copydoc CinematicRuntimeService::Play */
    Result<SequencePlayerTransition> CinematicRuntimeService::Play(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.Play(handle);
        if (transition.HasError())
            return transition;
        if (transition.Value().signal == SequencePlaybackSignal::Started) {
            const auto policy = instance.suppressNextPlayBoundary ? SequenceCursorResetPolicy::SuppressCurrentBoundary
                                                                  : SequenceCursorResetPolicy::EmitCurrentBoundary;
            if (auto synchronized = SynchronizeCursor(instance, policy); synchronized.HasError())
                return Result<SequencePlayerTransition>::Failure(synchronized.ErrorValue());
            instance.suppressNextPlayBoundary = false;
        } else if (transition.Value().signal == SequencePlaybackSignal::Resumed) {
            if (auto synchronized = SynchronizeCursor(instance, SequenceCursorResetPolicy::SuppressCurrentBoundary);
                synchronized.HasError())
                return Result<SequencePlayerTransition>::Failure(synchronized.ErrorValue());
        }
        return transition;
    }

    /** @copydoc CinematicRuntimeService::Pause */
    Result<SequencePlayerTransition> CinematicRuntimeService::Pause(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.Pause(handle);
        if (transition.HasError())
            return transition;
        if (transition.Value().signal == SequencePlaybackSignal::Paused) {
            if (auto rebound = RebindCursorFence(instance); rebound.HasError())
                return Result<SequencePlayerTransition>::Failure(rebound.ErrorValue());
        }
        return transition;
    }

    /** @copydoc CinematicRuntimeService::Stop */
    Result<SequencePlayerTransition> CinematicRuntimeService::Stop(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        return slots_[slot.Value()].instance->player.Stop(handle);
    }

    /** @copydoc CinematicRuntimeService::FinishStop */
    Result<SequencePlayerTransition> CinematicRuntimeService::FinishStop(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.FinishStop(handle);
        if (transition.HasValue())
            ReleaseCoordination(instance);
        return transition;
    }

    /** @copydoc CinematicRuntimeService::Seek */
    Result<SequencePlayerTransition> CinematicRuntimeService::Seek(const SequencePlayerHandle &handle, const SequenceTime target) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.Seek(handle, target);
        if (transition.HasError())
            return transition;
        if (transition.Value().signal == SequencePlaybackSignal::Seeked) {
            if (auto synchronized = SynchronizeCursor(instance, SequenceCursorResetPolicy::SuppressCurrentBoundary);
                synchronized.HasError())
                return Result<SequencePlayerTransition>::Failure(synchronized.ErrorValue());
            instance.suppressNextPlayBoundary = true;
        }
        return transition;
    }

    /** @copydoc CinematicRuntimeService::SetPlaybackSpeed */
    Result<SequencePlayerTransition> CinematicRuntimeService::SetPlaybackSpeed(const SequencePlayerHandle &handle,
                                                                               const SequencePlaybackRate rate) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.SetPlaybackSpeed(handle, rate);
        if (transition.HasError())
            return transition;
        if (transition.Value().signal == SequencePlaybackSignal::RateChanged) {
            if (auto rebound = RebindCursorFence(instance); rebound.HasError())
                return Result<SequencePlayerTransition>::Failure(rebound.ErrorValue());
        }
        return transition;
    }

    /** @copydoc CinematicRuntimeService::Cancel */
    Result<void> CinematicRuntimeService::Cancel(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<void>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        const SequencePlaybackState state = instance.player.Snapshot().state;
        if (IsTerminal(state)) {
            ReleaseCoordination(instance);
            return Result<void>::Success();
        }
        if (state != SequencePlaybackState::Closing) {
            auto closing = instance.player.Close(handle);
            if (closing.HasError())
                return Result<void>::Failure(closing.ErrorValue());
        }
        if (auto finished = instance.player.FinishClose(handle); finished.HasError())
            return Result<void>::Failure(finished.ErrorValue());
        ReleaseCoordination(instance);
        return Result<void>::Success();
    }

    /** @copydoc CinematicRuntimeService::Fail */
    Result<SequencePlayerTransition> CinematicRuntimeService::Fail(const SequencePlayerHandle &handle) {
        auto slot = ResolveSlot(handle);
        if (slot.HasError())
            return Result<SequencePlayerTransition>::Failure(slot.ErrorValue());
        Instance &instance = *slots_[slot.Value()].instance;
        auto transition = instance.player.Fail(handle);
        if (transition.HasValue())
            ReleaseCoordination(instance);
        return transition;
    }

    /** @copydoc CinematicRuntimeService::ResolveSlot */
    Result<std::size_t> CinematicRuntimeService::ResolveSlot(const SequencePlayerHandle &handle) const {
        if (!handle.IsValid())
            return Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleInvalid);
        if (handle.session.stableValue != session_.stableValue)
            return Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleUnknown);
        if (handle.session.generation != session_.generation)
            return Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleStale);
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const Slot &slot = slots_[index];
            if (slot.instance.has_value()) {
                const SequencePlayerHandle current = slot.instance->player.Snapshot().handle;
                if (current.player.stableValue == handle.player.stableValue)
                    return current == handle ? Result<std::size_t>::Success(index)
                                             : Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleStale);
            }
            if (slot.hasRetiredHandle && slot.retiredHandle.player.stableValue == handle.player.stableValue)
                return Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleStale);
        }
        return Failed<std::size_t>(SequencePlaybackRuntimeErrors::HandleUnknown);
    }

    /** @copydoc CinematicRuntimeService::SynchronizeCursor */
    Result<void> CinematicRuntimeService::SynchronizeCursor(Instance &instance, const SequenceCursorResetPolicy resetPolicy) const {
        auto cursor = MakeSequenceFrameCursor(instance.player.Snapshot(), resetPolicy);
        if (cursor.HasError())
            return Result<void>::Failure(cursor.ErrorValue());
        instance.cursor = std::move(cursor).Value();
        return Result<void>::Success();
    }

    /** @copydoc CinematicRuntimeService::RebindCursorFence */
    Result<void> CinematicRuntimeService::RebindCursorFence(Instance &instance) const {
        const SequencePlayerSnapshot snapshot = instance.player.Snapshot();
        if (!snapshot.handle.IsValid() || snapshot.controlRevision == 0 || snapshot.position < 0 || snapshot.position > snapshot.duration)
            return Failed<void>(SequencePlaybackRuntimeErrors::RevisionExhausted);
        instance.cursor.controlFence = instance.player.CaptureFence();
        instance.cursor.position = snapshot.position;
        instance.cursor.rateRemainder = 0;
        return Result<void>::Success();
    }

    /** @copydoc CinematicRuntimeService::ValidateActivation */
    Result<void> CinematicRuntimeService::ValidateActivation(const SequencePlaybackActivation &activation) const {
        if (!activation.player.handle.IsValid() || activation.player.handle.session != session_)
            return Failed<void>(SequencePlaybackRuntimeErrors::SessionInvalid);
        if (auto blend = ValidateSequencePlaybackBlendSettings(activation.blend); blend.HasError())
            return blend;
        if (auto coordination = ValidateSequencePlaybackCoordinationSettings(activation.coordination); coordination.HasError())
            return coordination;
        if ((activation.coordination.pauseGameplay || activation.coordination.hideHud) &&
            (activation.coordinationHooks.acquire == nullptr || activation.coordinationHooks.release == nullptr))
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (activation.blend.restorePolicy == SequenceRestorePolicy::RestorePrePlayback && !activation.restore.has_value())
            return Failed<void>(SequencePlaybackRuntimeErrors::RestoreInvalid);
        if (activation.blend.blendIn.mode == SequenceBlendMode::Blend || activation.blend.blendOut.mode == SequenceBlendMode::Blend) {
            if (activation.plan.LoopMode() != SequenceLoopMode::Once || !activation.restore.has_value())
                return Failed<void>(SequencePlaybackRuntimeErrors::RestoreInvalid);
            for (const SequenceFrameTrackDescriptor &track : activation.plan.Tracks()) {
                if (std::ranges::count_if(activation.restore->Entries(), [&](const SequenceRestoreEntry &entry) {
                    return entry.track == track.track;
                }) != 1)
                    return Failed<void>(SequencePlaybackRuntimeErrors::RestoreInvalid);
            }
        }
        if (activation.plan.Duration() != activation.player.duration)
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (activation.plan.TrackCount() > budget_.maximumTracksPerPlayer || activation.plan.TrackCount() > MaximumFrameEvaluationTracks ||
            activation.plan.EventCount() + activation.plan.CameraCutCount() > budget_.maximumBoundaryOccurrences ||
            activation.plan.MaximumLoopCrossings() > budget_.maximumLoopCrossings ||
            activation.retainedBytes > budget_.maximumRetainedBytes)
            return Failed<void>(SequencePlaybackRuntimeErrors::CapacityExceeded);
        if (activation.authority.has_value() && activation.authority->Claims().size() > MaximumSequenceAuthorityClaims)
            return Failed<void>(SequencePlaybackRuntimeErrors::AuthorityConflict);
        if (activation.authority.has_value() &&
            !std::ranges::all_of(activation.authority->Claims(), [&](const SequenceAuthorityClaim &claim) {
            return claim.player == activation.player.handle;
        }))
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        if (activation.coordination.pauseGameplay && activation.authority.has_value() &&
            !std::ranges::all_of(activation.authority->Claims(), [](const SequenceAuthorityClaim &claim) {
            return claim.mode == CinematicClaimMode::ObserveOnly || claim.mode == CinematicClaimMode::PresentationOverlay;
        }))
            return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
        return Result<void>::Success();
    }

    /** @copydoc CinematicRuntimeService::AdmitActivation */
    Result<void> CinematicRuntimeService::AdmitActivation(const SequencePlaybackActivation &activation, SequenceEvaluationUsage &additional,
                                                          std::size_t &slotIndex) const {
        const std::size_t occurrenceCount = activation.plan.EventCount() + activation.plan.CameraCutCount();
        const bool blending =
            activation.blend.blendIn.mode == SequenceBlendMode::Blend || activation.blend.blendOut.mode == SequenceBlendMode::Blend;
        const std::uint64_t baselineBytes = blending ? activation.plan.TrackCount() * sizeof(float) : 0;
        if (activation.retainedBytes > std::numeric_limits<std::uint64_t>::max() - baselineBytes)
            return Failed<void>(SequencePlaybackRuntimeErrors::CapacityExceeded);
        additional = {1, static_cast<std::uint32_t>(activation.plan.TrackCount()), static_cast<std::uint32_t>(occurrenceCount),
                      activation.retainedBytes + baselineBytes, static_cast<std::uint32_t>(activation.plan.MaximumLoopCrossings())};
        if (auto admitted = AdmitSequenceEvaluationUsage(usage_, additional, budget_); admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        const auto freeSlot = std::ranges::find_if(slots_, [](const Slot &slot) {
            return !slot.instance.has_value();
        });
        if (freeSlot == slots_.end())
            return Failed<void>(SequencePlaybackRuntimeErrors::CapacityExceeded);
        slotIndex = static_cast<std::size_t>(std::ranges::distance(slots_.begin(), freeSlot));
        return Result<void>::Success();
    }

    /** @brief Acquires the activation's host-owned pause and HUD leases with rollback. */
    Result<void> CinematicRuntimeService::AcquireCoordinationLeases(const SequencePlaybackActivation &activation,
                                                                    std::optional<SequenceCoordinationLease> &gameplayPauseLease,
                                                                    std::optional<SequenceCoordinationLease> &hudSuppressionLease) const {
        const auto releaseLease = [&](std::optional<SequenceCoordinationLease> &lease) noexcept {
            if (lease.has_value()) {
                activation.coordinationHooks.release(activation.coordinationHooks.context, *lease);
                lease.reset();
            }
        };
        const auto acquireLease = [&](const SequenceCoordinationLeaseKind kind, std::optional<SequenceCoordinationLease> &destination) {
            auto acquired = activation.coordinationHooks.acquire(activation.coordinationHooks.context, activation.player.handle, kind);
            if (acquired.HasError())
                return Result<void>::Failure(acquired.ErrorValue());
            if (!acquired.Value().IsValid() || acquired.Value().player != activation.player.handle || acquired.Value().kind != kind)
                return Failed<void>(SequencePlaybackRuntimeErrors::ActivationInvalid);
            destination = std::move(acquired).Value();
            return Result<void>::Success();
        };
        if (activation.coordination.pauseGameplay) {
            if (auto acquired = acquireLease(SequenceCoordinationLeaseKind::GameplayPause, gameplayPauseLease); acquired.HasError())
                return Result<void>::Failure(acquired.ErrorValue());
        }
        if (activation.coordination.hideHud) {
            if (auto acquired = acquireLease(SequenceCoordinationLeaseKind::HudSuppression, hudSuppressionLease); acquired.HasError()) {
                releaseLease(gameplayPauseLease);
                return Result<void>::Failure(acquired.ErrorValue());
            }
        }
        return Result<void>::Success();
    }

    void CinematicRuntimeService::ReleaseCoordination(Instance &instance) const noexcept {
        if (instance.hudSuppressionLease.has_value()) {
            instance.coordinationHooks.release(instance.coordinationHooks.context, *instance.hudSuppressionLease);
            instance.hudSuppressionLease.reset();
        }
        if (instance.gameplayPauseLease.has_value()) {
            instance.coordinationHooks.release(instance.coordinationHooks.context, *instance.gameplayPauseLease);
            instance.gameplayPauseLease.reset();
        }
    }

    void CinematicRuntimeService::RecalculateMaximumLoopCrossings() noexcept {
        usage_.maximumLoopCrossings = 0;
        for (const Slot &slot : slots_) {
            if (slot.instance.has_value())
                usage_.maximumLoopCrossings =
                    std::max(usage_.maximumLoopCrossings, static_cast<std::uint32_t>(slot.instance->plan.MaximumLoopCrossings()));
        }
    }

    /** @copydoc CinematicRuntimeService::BeginShutdown */
    Result<void> CinematicRuntimeService::BeginShutdown() {
        admissionOpen_ = false;
        for (Slot &slot : slots_) {
            if (!slot.instance.has_value())
                continue;
            Instance &instance = *slot.instance;
            const SequencePlayerSnapshot snapshot = instance.player.Snapshot();
            if (IsTerminal(snapshot.state)) {
                ReleaseCoordination(instance);
                continue;
            }
            if (snapshot.state != SequencePlaybackState::Closing) {
                auto closing = instance.player.Close(snapshot.handle);
                if (closing.HasError())
                    return Result<void>::Failure(closing.ErrorValue());
            }
            if (auto finished = instance.player.FinishClose(snapshot.handle); finished.HasError())
                return Result<void>::Failure(finished.ErrorValue());
            ReleaseCoordination(instance);
        }
        return Result<void>::Success();
    }

    /** @copydoc CinematicRuntimeService::ServiceSnapshot */
    CinematicRuntimeServiceSnapshot CinematicRuntimeService::ServiceSnapshot() const noexcept {
        return {admissionOpen_, budget_, usage_};
    }

    /** @copydoc CinematicRuntimeService::ActivePlayerCount */
    std::size_t CinematicRuntimeService::ActivePlayerCount() const noexcept {
        return usage_.activePlayers;
    }
}  // namespace Horo::Cinematic
