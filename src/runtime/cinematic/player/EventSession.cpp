#include "Horo/Cinematic/EventSession.h"

#include "Horo/Cinematic/EventTrackErrors.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Cinematic {
    /** @copydoc CinematicEventSession::CinematicEventSession */
    CinematicEventSession::CinematicEventSession(CinematicRuntimeService &runtime, CinematicEventDispatcher &dispatcher)
        : runtime_(runtime), dispatcher_(dispatcher) {
        frames_.reserve(MaximumFramePlayers);
    }

    CinematicEventSession::~CinematicEventSession() {
        if (open_)
            (void)dispatcher_.AbortTick();
    }

    /** @copydoc CinematicEventSession::Activate */
    Result<SequencePlayerHandle> CinematicEventSession::Activate(SequencePlaybackActivation activation,
                                                                 std::shared_ptr<const CookedEventPlan> events,
                                                                 const std::int32_t priority) {
        if (open_ || !events || activation.coordination.clockSource != SequenceClockSource::CommittedSimulation)
            return Result<SequencePlayerHandle>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        std::uint64_t retained = events->Keys().size() * sizeof(CookedEventKey);
        for (const auto &key : events->Keys())
            retained += key.payload.capacity();
        if (retained > std::numeric_limits<std::uint64_t>::max() - activation.retainedBytes)
            return Result<SequencePlayerHandle>::Failure(MakeError(EventTrackErrors::CookCapacityExceeded));
        activation.retainedBytes += retained;
        const auto player = activation.player.handle;
        if (auto admitted = dispatcher_.Activate(player, std::move(events), activation.plan, priority); admitted.HasError())
            return Result<SequencePlayerHandle>::Failure(admitted.ErrorValue());
        auto active = runtime_.Activate(std::move(activation));
        if (active.HasError())
            (void)dispatcher_.CancelPlayer(player);
        return active;
    }

    /** @copydoc CinematicEventSession::Stop */
    Result<SequencePlayerTransition> CinematicEventSession::Stop(const SequencePlayerHandle &player) {
        if (open_)
            return Result<SequencePlayerTransition>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        auto stopped = runtime_.Stop(player);
        if (stopped.HasError())
            return stopped;
        if (auto closed = dispatcher_.StopPlayer(player); closed.HasError())
            return Result<SequencePlayerTransition>::Failure(closed.ErrorValue());
        return stopped;
    }

    /** @copydoc CinematicEventSession::Cancel */
    Result<void> CinematicEventSession::Cancel(const SequencePlayerHandle &player) {
        if (open_)
            return Result<void>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        if (auto cancelled = runtime_.Cancel(player); cancelled.HasError())
            return cancelled;
        return dispatcher_.CancelPlayer(player);
    }

    /** @copydoc CinematicEventSession::Close */
    Result<void> CinematicEventSession::Close() {
        if (open_) {
            if (auto aborted = AbortTick(); aborted.HasError())
                return aborted;
        }
        if (auto shutdown = runtime_.BeginShutdown(); shutdown.HasError())
            return shutdown;
        return dispatcher_.Close();
    }

    /** @copydoc CinematicEventSession::BeginTick */
    Result<void> CinematicEventSession::BeginTick(const std::uint64_t tick) {
        if (open_)
            return Result<void>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        if (auto begun = dispatcher_.BeginTick(tick); begun.HasError())
            return begun;
        frames_.clear();
        open_ = true;
        failed_ = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventSession::Evaluate */
    Result<SequenceFrameEvaluationResult> CinematicEventSession::Evaluate(const SequencePlayerHandle &player, const SequenceTime delta,
                                                                          SequenceFrameScratch scratch, SequenceFrameHooks hooks) {
        if (!open_ || failed_ || frames_.size() == MaximumFramePlayers || std::ranges::any_of(frames_, [&](const Frame &frame) {
            return frame.player == player;
        })) {
            failed_ = true;
            return Result<SequenceFrameEvaluationResult>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        }
        hooks.eventContext = BorrowedCallbackContext{&dispatcher_};
        hooks.eventStage = CinematicEventDispatcher::StageHook;
        auto slot = runtime_.ResolveSlot(player);
        if (slot.HasError()) {
            failed_ = true;
            return Result<SequenceFrameEvaluationResult>::Failure(slot.ErrorValue());
        }
        const auto sourceCursor = runtime_.slots_[slot.Value()].instance->cursor;
        SequenceFrameCursor cursor;
        auto result = runtime_.PrepareEventFrame(player, delta, cursor, scratch, hooks);
        if (result.HasError()) {
            failed_ = true;
            return result;
        }
        frames_.emplace_back(player, cursor, scratch, hooks, result.Value(), sourceCursor);
        return result;
    }

    /** @copydoc CinematicEventSession::CommitTick */
    Result<void> CinematicEventSession::CommitTick() {
        if (!open_ || failed_)
            return Result<void>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        for (const Frame &frame : frames_) {
            if (auto snapshot = runtime_.Snapshot(frame.player);
                snapshot.HasError() || snapshot.Value().state != SequencePlaybackState::Playing ||
                snapshot.Value().controlRevision != frame.cursor.controlFence.controlRevision ||
                snapshot.Value().position != frame.result.previousPosition) {
                failed_ = true;
                return Result<void>::Failure(MakeError(EventTrackErrors::StaleBinding));
            }
            const auto &instance = *runtime_.slots_[runtime_.ResolveSlot(frame.player).Value()].instance;
            if (instance.cursor != frame.sourceCursor) {
                failed_ = true;
                return Result<void>::Failure(MakeError(EventTrackErrors::StaleBinding));
            }
            auto candidate = instance.player;
            if (auto published = candidate.CommitEvaluationPosition(frame.cursor.controlFence, frame.result.position);
                published.HasError()) {
                failed_ = true;
                return Result<void>::Failure(published.ErrorValue());
            }
            if (frame.result.reachedEnd) {
                if (auto stopping = candidate.Stop(frame.player); stopping.HasError()) {
                    failed_ = true;
                    return Result<void>::Failure(stopping.ErrorValue());
                }
                auto stopped = candidate.FinishStop(frame.player);
                if (stopped.HasError()) {
                    failed_ = true;
                    return Result<void>::Failure(stopped.ErrorValue());
                }
            }
        }
        if (auto committed = dispatcher_.CommitTick(); committed.HasError())
            return committed;
        for (const Frame &frame : frames_) {
            runtime_.PublishEventFrame(frame.player, frame.cursor, frame.scratch, frame.hooks, frame.result);
            if (frame.result.reachedEnd)
                (void)dispatcher_.StopPlayer(frame.player);
        }
        frames_.clear();
        open_ = false;
        return Result<void>::Success();
    }

    /** @copydoc CinematicEventSession::AbortTick */
    Result<void> CinematicEventSession::AbortTick() {
        if (!open_)
            return Result<void>::Failure(MakeError(EventTrackErrors::DispatchStateInvalid));
        if (auto aborted = dispatcher_.AbortTick(); aborted.HasError())
            return aborted;
        frames_.clear();
        open_ = false;
        failed_ = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Cinematic
