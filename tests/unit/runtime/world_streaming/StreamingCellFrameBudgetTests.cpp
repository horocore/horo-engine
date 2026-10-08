#include "StreamingCellDirectionTestSupport.h"

namespace Horo::WorldStreaming {
    using DirectionTestSupport::Add;
    using DirectionTestSupport::Config;
    using DirectionTestSupport::Demand;
    using DirectionTestSupport::Forward;
    using DirectionTestSupport::Participant;
    using DirectionTestSupport::ParticipantLog;
    using DirectionTestSupport::Participants;
    using DirectionTestSupport::Prepared;
    using DirectionTestSupport::Requirement;
    using DirectionTestSupport::Revision;
    using DirectionTestSupport::Scheduler;
    using TestSupport::IdentityFrom;
    using TestSupport::RequireError;

    TEST_CASE("Retirement resumes across bounded frames without releasing pending charges or batching destruction",
              "[unit][world_streaming][direction][frame_budget]") {
        for (const auto reason : {StreamingCellOperationTransition::Cancel, StreamingCellOperationTransition::Fail,
                                  StreamingCellOperationTransition::Replace, StreamingCellOperationTransition::Shutdown}) {
            auto scheduler = Scheduler();
            const auto config = Config();
            ParticipantLog log;
            Participants participants;
            Add(participants, log, config, 30).ready = true;
            Add(participants, log, config, 10).ready = true;
            auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
            REQUIRE(owner.Interrupt(owner.Operation().Handle(), reason).HasValue());
            scheduler.BeginShutdown();
            auto limits = StreamingOwnerFrameLimits{scheduler.Owner(), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                    IdentityFrom<StreamingOwnerFrameId>(1), 10, 1};
            auto frame = StreamingOwnerFrameBudget::Create(limits).Value();
            REQUIRE(owner.PollRetirement(frame, 0).Value().State() == StreamingCellOperationState::Retiring);
            REQUIRE(log.started == std::vector<std::uint64_t>{30});
            REQUIRE(log.destroyed == std::vector<std::uint64_t>{30});
            REQUIRE(log.leases == 1);
            REQUIRE(scheduler.ReservedCapacityUnits() == 5);
            REQUIRE(owner.PollRetirement(frame, 0).Value().State() == StreamingCellOperationState::Retiring);
            REQUIRE(log.started == std::vector<std::uint64_t>{30});
            auto successor = std::move(owner);
            limits.frame = IdentityFrom<StreamingOwnerFrameId>(2);
            auto nextFrame = StreamingOwnerFrameBudget::Create(limits).Value();
            REQUIRE(successor.PollRetirement(nextFrame, 0).Value().IsTerminal());
            REQUIRE(log.started == std::vector<std::uint64_t>{30, 10});
            REQUIRE(log.destroyed == std::vector<std::uint64_t>{30, 10});
            REQUIRE(log.leases == 0);
            REQUIRE(scheduler.ReservedCapacityUnits() == 0);
            REQUIRE(scheduler.State() == StreamingSchedulerAdmissionState::Closed);
            REQUIRE(nextFrame.ConsumedUnits() == 1);
        }
    }

    TEST_CASE("One shared frame prevents both cross-cell activation and retirement from bypassing its ceiling",
              "[unit][world_streaming][direction][frame_budget]") {
        auto scheduler = Scheduler(2, 10);
        const auto retiringConfig = Config(StreamingCellOperationKind::Retire);
        const auto activatingConfig = Config(StreamingCellOperationKind::Activate, 2);
        ParticipantLog retiredLog;
        ParticipantLog activeLog;
        Participants retiredParticipants;
        Participants activeParticipants;
        Add(retiredParticipants, retiredLog, retiringConfig, 1).ready = true;
        Add(activeParticipants, activeLog, activatingConfig, 1);
        auto retiring = StreamingCellDirectionOwner::Create(scheduler, retiringConfig, retiredParticipants).Value();
        auto activating = StreamingCellDirectionOwner::Create(scheduler, activatingConfig, activeParticipants).Value();
        Forward(retiring, StreamingCellOperationTransition::BeginRetirement);
        Forward(activating, StreamingCellOperationTransition::BeginPreparation);
        Forward(activating, StreamingCellOperationTransition::BeginActivation);
        auto transaction = Prepared(activating, activeLog);
        auto limits = StreamingOwnerFrameLimits{scheduler.Owner(), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                IdentityFrom<StreamingOwnerFrameId>(1), 10, 1};
        auto frame = StreamingOwnerFrameBudget::Create(limits).Value();
        auto foreignLimits = limits;
        foreignLimits.owner = IdentityFrom<StreamingSchedulerLedgerId>(2);
        auto foreign = StreamingOwnerFrameBudget::Create(foreignLimits).Value();
        RequireError(activating.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, foreign,
                                                 0),
                     WorldStreamingErrors::OwnerFrameStale);
        REQUIRE_FALSE(activeLog.published);
        REQUIRE(foreign.ConsumedUnits() == 0);
        REQUIRE(retiring.PollRetirement(frame, 0).Value().IsTerminal());
        RequireError(activating.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, frame, 0),
                     WorldStreamingErrors::OwnerFrameDeferred);
        REQUIRE_FALSE(activeLog.published);
        REQUIRE(transaction.State() == StreamingCellActivationState::Prepared);
        REQUIRE(scheduler.ReservedCapacityUnits() == 5);
        limits.frame = IdentityFrom<StreamingOwnerFrameId>(2);
        auto nextFrame = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(activating.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, nextFrame, 0)
                    .HasValue());
        REQUIRE(activeLog.published);
        REQUIRE(scheduler.ReservedCapacityUnits() == 0);
        auto resident = activating.TakeSucceededParticipants().Value();
        REQUIRE(resident.size() == 1);
    }

    TEST_CASE("Pending and failed retirement polls consume work without proving cleanup",
              "[unit][world_streaming][direction][frame_budget][failure]") {
        auto scheduler = Scheduler();
        const auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &participant = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        REQUIRE(owner.Interrupt(owner.Operation().Handle(), StreamingCellOperationTransition::Shutdown).HasValue());
        auto limits = StreamingOwnerFrameLimits{scheduler.Owner(), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                IdentityFrom<StreamingOwnerFrameId>(1), 10, 1};
        auto frame = StreamingOwnerFrameBudget::Create(limits).Value();
        participant.failPoll = true;
        RequireError(owner.PollRetirement(frame, 0), WorldStreamingErrors::CellAssetRequestUnavailable);
        REQUIRE(frame.ConsumedUnits() == 1);
        REQUIRE(log.leases == 1);
        REQUIRE(scheduler.ReservedCount() == 1);
        participant.failPoll = false;
        participant.ready = true;
        REQUIRE(owner.PollRetirement(frame, 0).Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(log.destroyed.empty());
        limits.frame = IdentityFrom<StreamingOwnerFrameId>(2);
        auto nextFrame = StreamingOwnerFrameBudget::Create(limits).Value();
        REQUIRE(owner.PollRetirement(nextFrame, 0).Value().IsTerminal());
        REQUIRE(log.started == std::vector<std::uint64_t>{1});
        REQUIRE(log.destroyed == std::vector<std::uint64_t>{1});
    }

    TEST_CASE("Foreign frame and oversized retirement step invoke no callbacks or resource release",
              "[unit][world_streaming][direction][frame_budget][failure]") {
        auto scheduler = Scheduler();
        const auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &participant = Add(participants, log, config, 1);
        participant.cost = 20;
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        REQUIRE(owner.Interrupt(owner.Operation().Handle(), StreamingCellOperationTransition::Cancel).HasValue());
        auto limits = StreamingOwnerFrameLimits{IdentityFrom<StreamingSchedulerLedgerId>(2), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                                IdentityFrom<StreamingOwnerFrameId>(1), 10, 1};
        auto foreign = StreamingOwnerFrameBudget::Create(limits).Value();
        RequireError(owner.PollRetirement(foreign, 0), WorldStreamingErrors::OwnerFrameStale);
        limits.owner = scheduler.Owner();
        auto small = StreamingOwnerFrameBudget::Create(limits).Value();
        RequireError(owner.PollRetirement(small, 0), WorldStreamingErrors::OwnerFrameCapacityExceeded);
        REQUIRE(log.started.empty());
        REQUIRE(log.destroyed.empty());
        REQUIRE(log.leases == 1);
        REQUIRE(scheduler.ReservedCount() == 1);
        limits.maximumNanoseconds = 20;
        auto barrier = StreamingOwnerFrameBudget::Create(limits).Value();
        participant.ready = true;
        REQUIRE(owner.PollRetirement(barrier, 0).Value().IsTerminal());
    }

}  // namespace Horo::WorldStreaming
