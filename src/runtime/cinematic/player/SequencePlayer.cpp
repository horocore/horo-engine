#include "Horo/Cinematic/SequencePlayer.h"

#include "Horo/Cinematic/SequencePlayerErrors.h"

#include <limits>

namespace Horo::Cinematic {
    namespace {
        constexpr std::int64_t MaximumRateMagnitude = 1'024;

        template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsRateValid(const SequencePlaybackRate rate) noexcept {
            if (rate.denominator == 0)
                return false;
            const auto numerator = static_cast<std::int64_t>(rate.numerator);
            const auto denominator = static_cast<std::int64_t>(rate.denominator);
            return numerator >= -MaximumRateMagnitude * denominator && numerator <= MaximumRateMagnitude * denominator;
        }

        [[nodiscard]] constexpr bool IsControllable(const SequencePlaybackState state) noexcept {
            using enum SequencePlaybackState;
            return state == Ready || state == Playing || state == Paused;
        }
    }  // namespace

    /** @copydoc SequencePlayer::Create */
    Result<SequencePlayer> SequencePlayer::Create(const SequencePlayerDescriptor &descriptor) {
        if (!descriptor.handle.IsValid())
            return Failed<SequencePlayer>(SequencePlayerErrors::HandleInvalid);
        if (descriptor.duration <= 0 || descriptor.initialPosition < 0 || descriptor.initialPosition > descriptor.duration)
            return Failed<SequencePlayer>(SequencePlayerErrors::TimeInvalid);
        if (!IsRateValid(descriptor.initialRate))
            return Failed<SequencePlayer>(SequencePlayerErrors::RateInvalid);
        return Result<SequencePlayer>::Success(SequencePlayer{descriptor});
    }

    /** @copydoc SequencePlayer::Snapshot */
    SequencePlayerSnapshot SequencePlayer::Snapshot() const noexcept {
        return snapshot_;
    }

    /** @copydoc SequencePlayer::CaptureFence */
    SequencePlayerOperationFence SequencePlayer::CaptureFence() const noexcept {
        return {snapshot_.handle, snapshot_.controlRevision};
    }

    /** @copydoc SequencePlayer::ValidateFence */
    Result<void> SequencePlayer::ValidateFence(const SequencePlayerOperationFence &fence) const {
        if (const Result<void> handle = ValidateHandle(fence.handle); handle.HasError())
            return handle;
        if (fence.controlRevision == 0 || fence.controlRevision != snapshot_.controlRevision)
            return Failed<void>(SequencePlayerErrors::HandleStale);
        return Result<void>::Success();
    }

    /** @copydoc SequencePlayer::Play */
    Result<SequencePlayerTransition> SequencePlayer::Play(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state == SequencePlaybackState::Playing)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (snapshot_.state == SequencePlaybackState::Ready)
            return Change(handle, SequencePlaybackState::Playing, SequencePlaybackSignal::Started,
                          SequenceEventTransitionPolicy::Unchanged);
        if (snapshot_.state == SequencePlaybackState::Paused)
            return Change(handle, SequencePlaybackState::Playing, SequencePlaybackSignal::Resumed,
                          SequenceEventTransitionPolicy::Unchanged);
        return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
    }

    /** @copydoc SequencePlayer::Pause */
    Result<SequencePlayerTransition> SequencePlayer::Pause(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state == SequencePlaybackState::Paused)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (snapshot_.state != SequencePlaybackState::Playing)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Paused, SequencePlaybackSignal::Paused, SequenceEventTransitionPolicy::Unchanged);
    }

    /** @copydoc SequencePlayer::Stop */
    Result<SequencePlayerTransition> SequencePlayer::Stop(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state == SequencePlaybackState::Stopping)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (!IsControllable(snapshot_.state))
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Stopping, SequencePlaybackSignal::StopRequested,
                      SequenceEventTransitionPolicy::CloseAndDrainAdmitted);
    }

    /** @copydoc SequencePlayer::FinishStop */
    Result<SequencePlayerTransition> SequencePlayer::FinishStop(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state != SequencePlaybackState::Stopping)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Stopped, SequencePlaybackSignal::Stopped, SequenceEventTransitionPolicy::Unchanged);
    }

    /** @copydoc SequencePlayer::Seek */
    Result<SequencePlayerTransition> SequencePlayer::Seek(const SequencePlayerHandle &handle, const SequenceTime target) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (target < 0 || target > snapshot_.duration)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TimeInvalid);
        SequencePlayerSnapshot requested = snapshot_;
        requested.position = target;
        return ApplyValueChange(handle, requested, SequencePlaybackSignal::Seeked, SequenceEventTransitionPolicy::ResetWithoutDispatch);
    }

    /** @copydoc SequencePlayer::CommitEvaluationPosition */
    Result<SequencePlayerTransition> SequencePlayer::CommitEvaluationPosition(const SequencePlayerOperationFence &fence,
                                                                              const SequenceTime position) {
        if (const Result<void> validation = ValidateFence(fence); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state != SequencePlaybackState::Playing)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        if (position < 0 || position > snapshot_.duration)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TimeInvalid);
        if (position == snapshot_.position)
            return Result<SequencePlayerTransition>::Success(NoChange());
        const SequencePlayerSnapshot previous = snapshot_;
        snapshot_.position = position;
        return Result<SequencePlayerTransition>::Success(
            TransitionFrom(previous, SequencePlaybackSignal::Advanced, SequenceEventTransitionPolicy::Unchanged));
    }

    /** @copydoc SequencePlayer::SetPlaybackSpeed */
    Result<SequencePlayerTransition> SequencePlayer::SetPlaybackSpeed(const SequencePlayerHandle &handle, const SequencePlaybackRate rate) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (!IsRateValid(rate))
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::RateInvalid);
        SequencePlayerSnapshot requested = snapshot_;
        requested.rate = rate;
        return ApplyValueChange(handle, requested, SequencePlaybackSignal::RateChanged, SequenceEventTransitionPolicy::Unchanged);
    }

    /** @copydoc SequencePlayer::Close */
    Result<SequencePlayerTransition> SequencePlayer::Close(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state == SequencePlaybackState::Closing)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (snapshot_.state == SequencePlaybackState::Stopped || snapshot_.state == SequencePlaybackState::Failed)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Closing, SequencePlaybackSignal::Closing,
                      SequenceEventTransitionPolicy::CloseAndDiscardPending);
    }

    /** @copydoc SequencePlayer::FinishClose */
    Result<SequencePlayerTransition> SequencePlayer::FinishClose(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state != SequencePlaybackState::Closing)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Stopped, SequencePlaybackSignal::Stopped, SequenceEventTransitionPolicy::Unchanged);
    }

    /** @copydoc SequencePlayer::Fail */
    Result<SequencePlayerTransition> SequencePlayer::Fail(const SequencePlayerHandle &handle) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.state == SequencePlaybackState::Failed)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (snapshot_.state == SequencePlaybackState::Stopped || snapshot_.state == SequencePlaybackState::Closing)
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::TransitionInvalid);
        return Change(handle, SequencePlaybackState::Failed, SequencePlaybackSignal::Failed,
                      SequenceEventTransitionPolicy::CloseAndDiscardPending);
    }

    SequencePlayer::SequencePlayer(const SequencePlayerDescriptor &descriptor) noexcept
        : snapshot_{descriptor.handle,   SequencePlaybackState::Ready, descriptor.initialPosition,
                    descriptor.duration, descriptor.initialRate,       1} {}

    Result<void> SequencePlayer::ValidateHandle(const SequencePlayerHandle &handle) const {
        if (!handle.IsValid())
            return Failed<void>(SequencePlayerErrors::HandleInvalid);
        if (handle.session.stableValue != snapshot_.handle.session.stableValue ||
            handle.player.stableValue != snapshot_.handle.player.stableValue)
            return Failed<void>(SequencePlayerErrors::HandleUnknown);
        if (handle.session.generation != snapshot_.handle.session.generation ||
            handle.player.generation != snapshot_.handle.player.generation)
            return Failed<void>(SequencePlayerErrors::HandleStale);
        return Result<void>::Success();
    }

    Result<void> SequencePlayer::ValidateControllableHandle(const SequencePlayerHandle &handle) const {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return validation;
        if (!IsControllable(snapshot_.state))
            return Failed<void>(SequencePlayerErrors::TransitionInvalid);
        return Result<void>::Success();
    }

    Result<SequencePlayerTransition> SequencePlayer::Change(const SequencePlayerHandle &handle, const SequencePlaybackState state,
                                                            const SequencePlaybackSignal signal,
                                                            const SequenceEventTransitionPolicy eventPolicy) {
        if (const Result<void> validation = ValidateHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (snapshot_.controlRevision == std::numeric_limits<std::uint64_t>::max())
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::RevisionExhausted);
        const SequencePlayerSnapshot previous = snapshot_;
        snapshot_.state = state;
        ++snapshot_.controlRevision;
        return Result<SequencePlayerTransition>::Success(TransitionFrom(previous, signal, eventPolicy));
    }

    Result<SequencePlayerTransition> SequencePlayer::ApplyValueChange(const SequencePlayerHandle &handle,
                                                                      const SequencePlayerSnapshot &requested,
                                                                      const SequencePlaybackSignal signal,
                                                                      const SequenceEventTransitionPolicy eventPolicy) {
        if (const Result<void> validation = ValidateControllableHandle(handle); validation.HasError())
            return Result<SequencePlayerTransition>::Failure(validation.ErrorValue());
        if (requested.position == snapshot_.position && requested.rate == snapshot_.rate)
            return Result<SequencePlayerTransition>::Success(NoChange());
        if (snapshot_.controlRevision == std::numeric_limits<std::uint64_t>::max())
            return Failed<SequencePlayerTransition>(SequencePlayerErrors::RevisionExhausted);
        const SequencePlayerSnapshot previous = snapshot_;
        snapshot_.position = requested.position;
        snapshot_.rate = requested.rate;
        ++snapshot_.controlRevision;
        return Result<SequencePlayerTransition>::Success(TransitionFrom(previous, signal, eventPolicy));
    }

    SequencePlayerTransition SequencePlayer::NoChange() const noexcept {
        return {snapshot_.state,
                snapshot_.state,
                SequencePlaybackSignal::None,
                SequenceEventTransitionPolicy::Unchanged,
                snapshot_.position,
                snapshot_.position,
                snapshot_.rate,
                snapshot_.rate,
                snapshot_.controlRevision};
    }

    SequencePlayerTransition SequencePlayer::TransitionFrom(const SequencePlayerSnapshot &previous, const SequencePlaybackSignal signal,
                                                            const SequenceEventTransitionPolicy eventPolicy) const noexcept {
        return {previous.state, snapshot_.state,          signal, eventPolicy, previous.position, snapshot_.position, previous.rate,
                snapshot_.rate, snapshot_.controlRevision};
    }
}  // namespace Horo::Cinematic
