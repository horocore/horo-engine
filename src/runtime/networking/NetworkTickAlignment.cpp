#include "Horo/Network/NetworkTickAlignment.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>

namespace Horo::Network {
    namespace {
        [[nodiscard]] Result<std::uint64_t> Project(const std::uint64_t serverTick, const std::uint64_t sampleLocalTick,
                                                    const std::uint64_t targetLocalTick) {
            if (targetLocalTick < sampleLocalTick)
                return Result<std::uint64_t>::Failure(MakeError(NetworkErrors::NetworkClockInvalid));
            const std::uint64_t elapsed = targetLocalTick - sampleLocalTick;
            if (serverTick > std::numeric_limits<std::uint64_t>::max() - elapsed)
                return Result<std::uint64_t>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
            return Result<std::uint64_t>::Success(serverTick + elapsed);
        }

        [[nodiscard]] constexpr std::uint64_t Distance(const std::uint64_t left, const std::uint64_t right) noexcept {
            return left >= right ? left - right : right - left;
        }
    }  // namespace

    NetworkTickAlignment::NetworkTickAlignment(const ConnectionHandle connection, const NetworkOperationGeneration session,
                                               const NetworkTickAlignmentPolicy policy, NetworkDebugger *debugger) noexcept
        : debugger_(debugger), diagnosticSource_(debugger ? debugger->Source() : NetworkDiagnosticSource{}), connection_(connection),
          session_(session), policy_(policy) {}

    /** @copydoc NetworkTickAlignment::Create */
    Result<NetworkTickAlignment> NetworkTickAlignment::Create(const ConnectionHandle connection, const NetworkOperationGeneration session,
                                                              const NetworkTickAlignmentPolicy policy, NetworkDebugger *debugger) {
        if (!connection.IsValid() || !session.IsValid() || policy.maximumRoundTripTicks == 0 || policy.staleAfterTicks == 0 ||
            policy.retainedSamples == 0 || policy.retainedSamples > MaximumNetworkClockSamples)
            return Result<NetworkTickAlignment>::Failure(MakeError(NetworkErrors::NetworkClockInvalid));
        return Result<NetworkTickAlignment>::Success(NetworkTickAlignment{connection, session, policy, debugger});
    }

    void NetworkTickAlignment::ClearSamples() noexcept {
        sampleCount_ = 0;
        latestSampleLocalTick_ = 0;
        latestRoundTripTicks_ = 0;
    }

    /** @copydoc NetworkTickAlignment::Observe */
    Result<void> NetworkTickAlignment::Observe(const NetworkClockSample &sample) {
        if (state_ != NetworkTickAlignmentState::Tracking)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        if (sample.connection != connection_ || sample.session != session_ || sample.clockEpoch != clockEpoch_ || sample.sequence == 0 ||
            sample.sequence <= lastSequence_ || (lastSequence_ != 0 && sample.serverSendTick <= lastServerSendTick_))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockSampleStale));
        if (hasMapping_ && sample.localReceiptTick < localTick_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockSampleStale));
        if (sample.roundTripTicks > policy_.maximumRoundTripTicks || (hasMapping_ && sample.localReceiptTick > localTick_) ||
            (!hasMapping_ && sample.localReceiptTick == std::numeric_limits<std::uint64_t>::max()))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockInvalid));
        if (sampleCount_ != 0 && sample.localReceiptTick == latestSampleLocalTick_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockSampleStale));

        const std::uint64_t oneWayTicks = sample.roundTripTicks / 2 + sample.roundTripTicks % 2;
        if (sample.serverSendTick > std::numeric_limits<std::uint64_t>::max() - oneWayTicks)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
        const Anchor anchor{sample.localReceiptTick, sample.serverSendTick + oneWayTicks};

        if (!hasMapping_) {
            localTick_ = anchor.localTick;
            serverTick_ = anchor.serverTick;
            hasMapping_ = true;
        }
        if (sampleCount_ == policy_.retainedSamples) {
            std::ranges::move(anchors_.begin() + 1, anchors_.begin() + static_cast<std::ptrdiff_t>(sampleCount_), anchors_.begin());
            --sampleCount_;
        }
        anchors_[sampleCount_++] = anchor;
        lastSequence_ = sample.sequence;
        lastServerSendTick_ = sample.serverSendTick;
        latestSampleLocalTick_ = sample.localReceiptTick;
        latestRoundTripTicks_ = sample.roundTripTicks;
        return Result<void>::Success();
    }

    Result<std::uint64_t> NetworkTickAlignment::MedianAt(const std::uint64_t localTick) const {
        if (sampleCount_ == 0)
            return Result<std::uint64_t>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        std::array<std::uint64_t, MaximumNetworkClockSamples> projected{};
        for (std::size_t index = 0; index < sampleCount_; ++index) {
            auto value = Project(anchors_[index].serverTick, anchors_[index].localTick, localTick);
            if (value.HasError())
                return value;
            projected[index] = value.Value();
        }
        std::ranges::sort(projected.begin(), projected.begin() + static_cast<std::ptrdiff_t>(sampleCount_));
        return Result<std::uint64_t>::Success(projected[(sampleCount_ - 1) / 2]);
    }

    /** @copydoc NetworkTickAlignment::Advance */
    Result<NetworkTickAlignmentSnapshot> NetworkTickAlignment::Advance(const std::uint64_t localCommittedTick) {
        if (state_ != NetworkTickAlignmentState::Tracking || !hasMapping_)
            return Result<NetworkTickAlignmentSnapshot>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        if (localTick_ == std::numeric_limits<std::uint64_t>::max())
            return Result<NetworkTickAlignmentSnapshot>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
        if (localCommittedTick != localTick_ + 1)
            return Result<NetworkTickAlignmentSnapshot>::Failure(MakeError(NetworkErrors::NetworkClockInvalid));
        if (serverTick_ == std::numeric_limits<std::uint64_t>::max())
            return Result<NetworkTickAlignmentSnapshot>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));

        const std::uint64_t ordinaryNext = serverTick_ + 1;
        std::uint64_t next = ordinaryNext;
        if (sampleCount_ != 0 && localCommittedTick - latestSampleLocalTick_ <= policy_.staleAfterTicks) {
            auto target = MedianAt(localCommittedTick);
            if (target.HasError())
                return Result<NetworkTickAlignmentSnapshot>::Failure(target.ErrorValue());
            if (target.Value() < ordinaryNext)
                next = serverTick_;
            else if (target.Value() > ordinaryNext && ordinaryNext < std::numeric_limits<std::uint64_t>::max())
                next = ordinaryNext + 1;
        }
        localTick_ = localCommittedTick;
        serverTick_ = next;
        const auto snapshot = Snapshot();
        if (debugger_)
            (void)debugger_->Observe(diagnosticSource_,
                                     NetworkPredictionRecord{snapshot.localTick, snapshot.serverTick, snapshot.sampleAgeTicks,
                                                             snapshot.hasMapping, snapshot.quality == NetworkTimingQuality::Stale});
        return Result<NetworkTickAlignmentSnapshot>::Success(snapshot);
    }

    /** @copydoc NetworkTickAlignment::Pause */
    Result<void> NetworkTickAlignment::Pause() {
        if (state_ != NetworkTickAlignmentState::Tracking)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        if (clockEpoch_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
        ++clockEpoch_;
        ClearSamples();
        state_ = NetworkTickAlignmentState::Paused;
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::Resume */
    Result<void> NetworkTickAlignment::Resume() {
        if (state_ != NetworkTickAlignmentState::Paused)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        state_ = NetworkTickAlignmentState::Tracking;
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::Suspend */
    Result<void> NetworkTickAlignment::Suspend() {
        if (state_ != NetworkTickAlignmentState::Tracking && state_ != NetworkTickAlignmentState::Paused)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        if (clockEpoch_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
        wasPausedBeforeSuspend_ = state_ == NetworkTickAlignmentState::Paused;
        ++clockEpoch_;
        ClearSamples();
        state_ = NetworkTickAlignmentState::Suspended;
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::ResumeFromSuspend */
    Result<void> NetworkTickAlignment::ResumeFromSuspend() {
        if (state_ != NetworkTickAlignmentState::Suspended)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        state_ = wasPausedBeforeSuspend_ ? NetworkTickAlignmentState::Paused : NetworkTickAlignmentState::Tracking;
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::Disconnect */
    Result<void> NetworkTickAlignment::Disconnect() {
        if (state_ == NetworkTickAlignmentState::Shutdown)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockUnavailable));
        state_ = NetworkTickAlignmentState::Disconnected;
        ClearSamples();
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::Replace */
    Result<void> NetworkTickAlignment::Replace(const ConnectionHandle connection, const NetworkOperationGeneration session) {
        if (state_ != NetworkTickAlignmentState::Disconnected || !connection.IsValid() || !session.IsValid() || session == session_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockSampleStale));
        if (connection.Slot() == connection_.Slot() && connection.Generation() != connection_.Generation() &&
            (connection_.Generation() == std::numeric_limits<std::uint32_t>::max() ||
             connection.Generation() != connection_.Generation() + 1))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockSampleStale));
        if (clockEpoch_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkClockOverflow));
        ++clockEpoch_;
        connection_ = connection;
        session_ = session;
        ClearSamples();
        lastSequence_ = 0;
        lastServerSendTick_ = 0;
        localTick_ = 0;
        serverTick_ = 0;
        hasMapping_ = false;
        wasPausedBeforeSuspend_ = false;
        state_ = NetworkTickAlignmentState::Tracking;
        return Result<void>::Success();
    }

    /** @copydoc NetworkTickAlignment::Shutdown */
    void NetworkTickAlignment::Shutdown() noexcept {
        state_ = NetworkTickAlignmentState::Shutdown;
        ClearSamples();
    }

    /** @copydoc NetworkTickAlignment::Snapshot */
    NetworkTickAlignmentSnapshot NetworkTickAlignment::Snapshot() const noexcept {
        NetworkTickAlignmentSnapshot snapshot;
        snapshot.connection = connection_;
        snapshot.session = session_;
        snapshot.clockEpoch = clockEpoch_;
        snapshot.localTick = localTick_;
        snapshot.serverTick = serverTick_;
        snapshot.retainedSamples = sampleCount_;
        snapshot.hasMapping = hasMapping_;
        snapshot.state = state_;
        if (sampleCount_ == 0)
            return snapshot;

        snapshot.sampleAgeTicks = localTick_ - latestSampleLocalTick_;
        snapshot.latestRoundTripTicks = latestRoundTripTicks_;
        if (snapshot.sampleAgeTicks == 0)
            snapshot.quality = NetworkTimingQuality::Tracking;
        else if (snapshot.sampleAgeTicks <= policy_.staleAfterTicks)
            snapshot.quality = NetworkTimingQuality::Holdover;
        else
            snapshot.quality = NetworkTimingQuality::Stale;
        if (sampleCount_ > 1) {
            const Anchor oldest = anchors_[0];
            const Anchor newest = anchors_[sampleCount_ - 1];
            snapshot.driftWindowTicks = newest.localTick - oldest.localTick;
            const std::uint64_t elapsed = snapshot.driftWindowTicks;
            const std::uint64_t expectedOldest = elapsed > std::numeric_limits<std::uint64_t>::max() - oldest.serverTick
                                                     ? std::numeric_limits<std::uint64_t>::max()
                                                     : oldest.serverTick + elapsed;
            snapshot.driftMagnitudeTicks = Distance(newest.serverTick, expectedOldest);
            snapshot.serverClockAhead = newest.serverTick > expectedOldest;
        }
        return snapshot;
    }
}  // namespace Horo::Network
