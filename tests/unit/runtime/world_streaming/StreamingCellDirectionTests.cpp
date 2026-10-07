#include "Horo/WorldStreaming/StreamingCellDirection.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "StreamingCellDirectionTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

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

    TEST_CASE("Direction reversal retains leases and capacity until ordered exact retirement", "[unit][world_streaming][direction]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &dependent = Add(participants, log, config, 30);
        auto &backing = Add(participants, log, config, 10);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        REQUIRE(participants.empty());
        Forward(owner, StreamingCellOperationTransition::BeginPreparation);
        const auto late = owner.Operation();
        Demand(owner, StreamingDesiredResidency::Unloaded);
        REQUIRE(log.revoked == std::vector<std::uint64_t>{30, 10});
        REQUIRE(owner.Operation().State() == StreamingCellOperationState::Retiring);
        RequireError(owner.Advance(late, StreamingCellOperationTransition::Complete), WorldStreamingErrors::CellDirectionStale);
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(log.started == std::vector<std::uint64_t>{30});
        REQUIRE(log.leases == 2);
        REQUIRE(scheduler.ReservedCapacityUnits() == 5);
        RequireError(scheduler.TryAdmit(Config(StreamingCellOperationKind::Load, 2).operation, 5, scheduler.Limits().concurrency.revision),
                     WorldStreamingErrors::SchedulerCapacityExceeded);
        Demand(owner, StreamingDesiredResidency::Activated);
        REQUIRE_FALSE(owner.RequiresFreshAttempt());
        dependent.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(log.started == std::vector<std::uint64_t>{30});
        REQUIRE(log.destroyed == std::vector<std::uint64_t>{30});
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().State() == StreamingCellOperationState::Retiring);
        REQUIRE(log.started == std::vector<std::uint64_t>{30, 10});
        REQUIRE(log.leases == 1);
        backing.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().Outcome() == StreamingCellOperationOutcome::Cancelled);
        REQUIRE(log.leases == 0);
        REQUIRE(scheduler.ReservedCapacityUnits() == 0);
        REQUIRE(owner.RequiresFreshAttempt());
        REQUIRE(owner.TakeTerminalResult().Value().Outcome() == StreamingCellOperationOutcome::Cancelled);
        RequireError(owner.TakeTerminalResult(), WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        RequireError(owner.PollRetirement(frameBudget, 0), WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        Participants freshParticipants;
        auto fresh = StreamingCellDirectionOwner::Create(scheduler, Config(StreamingCellOperationKind::Load, 2), freshParticipants).Value();
        Forward(fresh, StreamingCellOperationTransition::BeginPreparation);
        Forward(fresh, StreamingCellOperationTransition::Complete);
        REQUIRE(fresh.TakeTerminalResult().Value().Outcome() == StreamingCellOperationOutcome::Succeeded);
    }

    TEST_CASE("Direction retirement rejects stale acknowledgements and preserves domain errors and ownership",
              "[unit][world_streaming][direction][failure]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        Demand(owner, StreamingDesiredResidency::Unloaded);
        view.failPoll = true;
        RequireError(owner.PollRetirement(frameBudget, 0), WorldStreamingErrors::CellAssetRequestUnavailable);
        view.failPoll = false;
        view.ready = true;
        view.staleRevision = true;
        RequireError(owner.PollRetirement(frameBudget, 0), WorldStreamingErrors::CellDirectionStale);
        view.staleRevision = false;
        view.staleFence = true;
        RequireError(owner.PollRetirement(frameBudget, 0), WorldStreamingErrors::CellDirectionStale);
        REQUIRE(scheduler.ReservedCount() == 1);
        REQUIRE(log.leases == 1);
        REQUIRE(log.started == std::vector<std::uint64_t>{1});
        view.staleFence = false;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().IsTerminal());
        REQUIRE(log.leases == 0);
    }

    TEST_CASE("Direction demand validation and participant admission are transactional", "[unit][world_streaming][direction][admission]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        Add(participants, log, config, 1);
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::CellDirectionInvalid);
        REQUIRE(participants.size() == 2);
        REQUIRE(scheduler.ReservedCount() == 0);
        participants.pop_back();
        view.cost = 0;
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::CellDirectionInvalid);
        REQUIRE(scheduler.ReservedCount() == 0);
        view.cost = 10;
        auto extra = std::make_unique<Participant>(Requirement(2), config.operation.Handle(), log);
        participants.push_back(std::move(extra));
        config.maximumParticipants = 1;
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants),
                     WorldStreamingErrors::CellDirectionCapacityExceeded);
        participants.pop_back();
        const auto originalConfig = config;
        config.operation = Config(StreamingCellOperationKind::Load, 2).operation;
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::CellDirectionStale);
        config = originalConfig;
        config.maximumParticipants = 0;
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::CellDirectionInvalid);
        config.maximumParticipants = 1;
        config.desired = static_cast<StreamingDesiredResidency>(255);
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::CellDirectionUnsupported);
        config.desired = StreamingDesiredResidency::Activated;
        config.capacityUnits = 6;
        RequireError(StreamingCellDirectionOwner::Create(scheduler, config, participants), WorldStreamingErrors::SchedulerCapacityExceeded);
        REQUIRE(log.revoked.empty());
        config.capacityUnits = 5;
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        REQUIRE(owner.Interrupt(owner.Operation().Handle(), StreamingCellOperationTransition::Shutdown).HasValue());
        view.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().IsTerminal());
    }

    TEST_CASE("Direction demand rejects malformed and stale updates without changing ownership",
              "[unit][world_streaming][direction][admission]") {
        auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config();
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        const auto initial = owner.Operation();
        RequireError(owner.UpdateDemand(initial.Handle(), Revision(2), Revision(3), StreamingDesiredResidency::Unloaded),
                     WorldStreamingErrors::CellDirectionStale);
        RequireError(owner.UpdateDemand(initial.Handle(), Revision(1), Revision(1), StreamingDesiredResidency::Unloaded),
                     WorldStreamingErrors::CellDirectionStale);
        RequireError(owner.UpdateDemand(initial.Handle(), Revision(1), Revision(2), static_cast<StreamingDesiredResidency>(255)),
                     WorldStreamingErrors::CellDirectionUnsupported);
        RequireError(owner.UpdateDemand(initial.Handle(), {}, Revision(2), StreamingDesiredResidency::Unloaded),
                     WorldStreamingErrors::CellDirectionInvalid);
        REQUIRE(owner.Revision() == Revision(1));
        REQUIRE(owner.Operation().State() == StreamingCellOperationState::Admitted);
        REQUIRE(log.revoked.empty());
        REQUIRE(owner.Interrupt(initial.Handle(), StreamingCellOperationTransition::Shutdown).HasValue());
        view.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().IsTerminal());
    }

    TEST_CASE("Prepared activation rolls back on downward demand and cannot publish a late receipt",
              "[unit][world_streaming][direction][activation]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config(StreamingCellOperationKind::Activate);
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        Forward(owner, StreamingCellOperationTransition::BeginPreparation);
        Forward(owner, StreamingCellOperationTransition::BeginActivation);
        auto transaction = Prepared(owner, log);
        Demand(owner, StreamingDesiredResidency::Loaded);
        RequireError(owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, frameBudget,
                                            0),
                     WorldStreamingErrors::CellActivationLifecycleUnavailable);
        REQUIRE(log.rolledBack);
        REQUIRE_FALSE(log.published);
        REQUIRE(log.leases == 1);
        view.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().Outcome() == StreamingCellOperationOutcome::Cancelled);
        REQUIRE(owner.RequiresFreshAttempt());
    }

    TEST_CASE("Normal direction completion transfers successful participant lifetime and publishes once",
              "[unit][world_streaming][direction][activation]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config(StreamingCellOperationKind::Activate);
        ParticipantLog log;
        Participants participants;
        Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        Forward(owner, StreamingCellOperationTransition::BeginPreparation);
        Forward(owner, StreamingCellOperationTransition::BeginActivation);
        auto transaction = Prepared(owner, log);
        RequireError(owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::PreUpdate, frameBudget, 0),
                     WorldStreamingErrors::CellActivationSafePointUnavailable);
        REQUIRE_FALSE(log.published);
        REQUIRE(owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, frameBudget, 0)
                    .HasValue());
        REQUIRE(log.published);
        REQUIRE(scheduler.ReservedCount() == 0);
        REQUIRE(owner.TakeTerminalResult().Value().Outcome() == StreamingCellOperationOutcome::Succeeded);
        auto resident = owner.TakeSucceededParticipants().Value();
        REQUIRE(log.leases == 1);
        REQUIRE(log.revoked.empty());
        RequireError(owner.TakeSucceededParticipants(), WorldStreamingErrors::CellDirectionLifecycleUnavailable);
        resident.clear();
        REQUIRE(log.leases == 0);
    }

    TEST_CASE("Retirement never reverses and move shutdown and replacement retain the first outcome",
              "[unit][world_streaming][direction][lifecycle]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        for (const auto reason : {StreamingCellOperationTransition::Cancel, StreamingCellOperationTransition::Fail,
                                  StreamingCellOperationTransition::Replace, StreamingCellOperationTransition::Shutdown}) {
            auto scheduler = Scheduler();
            const auto config = Config();
            ParticipantLog log;
            Participants participants;
            auto &view = Add(participants, log, config, 1);
            auto source = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
            REQUIRE(source.Interrupt(source.Operation().Handle(), reason).HasValue());
            const auto disposition = source.Operation().Outcome();
            auto owner = std::move(source);
            RequireError(source.PollRetirement(frameBudget, 0), WorldStreamingErrors::CellDirectionLifecycleUnavailable);
            REQUIRE(owner.Interrupt(owner.Operation().Handle(), StreamingCellOperationTransition::Shutdown).HasValue());
            REQUIRE(owner.Operation().Outcome() == disposition);
            RequireError(owner.UpdateDemand(owner.Operation().Handle(), Revision(1), Revision(2), StreamingDesiredResidency::Activated),
                         WorldStreamingErrors::CellDirectionLifecycleUnavailable);
            scheduler.BeginShutdown();
            REQUIRE(scheduler.State() == StreamingSchedulerAdmissionState::Draining);
            REQUIRE(owner.PollRetirement(frameBudget, 0).Value().State() == StreamingCellOperationState::Retiring);
            REQUIRE(log.leases == 1);
            view.ready = true;
            REQUIRE(owner.PollRetirement(frameBudget, 0).Value().Outcome() == disposition);
            REQUIRE(scheduler.State() == StreamingSchedulerAdmissionState::Closed);
            REQUIRE_FALSE(owner.RequiresFreshAttempt());
        }
        auto scheduler = Scheduler();
        const auto config = Config(StreamingCellOperationKind::Retire);
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        Forward(owner, StreamingCellOperationTransition::BeginRetirement);
        Demand(owner, StreamingDesiredResidency::Loaded);
        REQUIRE(owner.Operation().State() == StreamingCellOperationState::Retiring);
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().State() == StreamingCellOperationState::Retiring);
        view.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().Outcome() == StreamingCellOperationOutcome::Succeeded);
        REQUIRE(owner.RequiresFreshAttempt());
    }

    TEST_CASE("Scheduler shutdown prevents prepared publication while exact cleanup remains routable",
              "[unit][world_streaming][direction][shutdown]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler();
        const auto config = Config(StreamingCellOperationKind::Activate);
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, config, 1);
        auto owner = StreamingCellDirectionOwner::Create(scheduler, config, participants).Value();
        Forward(owner, StreamingCellOperationTransition::BeginPreparation);
        Forward(owner, StreamingCellOperationTransition::BeginActivation);
        auto transaction = Prepared(owner, log);
        scheduler.BeginShutdown();
        RequireError(owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, frameBudget,
                                            0),
                     WorldStreamingErrors::CellActivationLifecycleUnavailable);
        REQUIRE(log.rolledBack);
        REQUIRE_FALSE(log.published);
        REQUIRE(owner.Interrupt(owner.Operation().Handle(), StreamingCellOperationTransition::Shutdown).HasValue());
        view.ready = true;
        REQUIRE(owner.PollRetirement(frameBudget, 0).Value().Outcome() == StreamingCellOperationOutcome::Shutdown);
        REQUIRE(scheduler.State() == StreamingSchedulerAdmissionState::Closed);
    }

    TEST_CASE("Retired direction evidence cannot change a successor residency generation", "[unit][world_streaming][direction][fencing]") {
        [[maybe_unused]] auto frameBudget =
            StreamingOwnerFrameBudget::Create({IdentityFrom<StreamingSchedulerLedgerId>(1), IdentityFrom<StreamingOwnerWorkRevision>(1),
                                               IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000})
                .Value();
        auto scheduler = Scheduler(2, 10);
        const auto authority = TestSupport::WorldOwner();
        auto residency = StreamingCellStateLedger::Create({authority, 2}).Value();
        auto oldConfig = Config();
        auto handle = oldConfig.operation.Handle();
        handle.fence.partition = authority.partition;
        handle.fence.epoch = authority.epoch;
        oldConfig.operation = StreamingCellOperation::Create(handle, StreamingCellOperationKind::Load).Value();
        ParticipantLog log;
        Participants participants;
        auto &view = Add(participants, log, oldConfig, 1);
        auto old = StreamingCellDirectionOwner::Create(scheduler, oldConfig, participants).Value();
        REQUIRE(residency.Apply(authority, old.Operation()).Value().state == StreamingCellState::Loading);
        Demand(old, StreamingDesiredResidency::Unloaded);
        REQUIRE(residency.Apply(authority, old.Operation()).Value().state == StreamingCellState::Evicting);
        auto freshConfig = oldConfig;
        handle.operation = IdentityFrom<StreamingCellOperationId>(2);
        handle.fence.generation = IdentityFrom<StreamingGeneration>(2);
        freshConfig.operation = StreamingCellOperation::Create(handle, StreamingCellOperationKind::Load).Value();
        Participants freshParticipants;
        auto fresh = StreamingCellDirectionOwner::Create(scheduler, freshConfig, freshParticipants).Value();
        REQUIRE(residency.Apply(authority, fresh.Operation()).Value().state == StreamingCellState::Loading);
        view.ready = true;
        REQUIRE(old.PollRetirement(frameBudget, 0).Value().IsTerminal());
        REQUIRE(residency.Apply(authority, old.Operation()).Value().state == StreamingCellState::Unloaded);
        REQUIRE(residency.Resolve(authority, fresh.Operation().Handle().fence).Value().state == StreamingCellState::Loading);
        Forward(fresh, StreamingCellOperationTransition::BeginPreparation);
        Forward(fresh, StreamingCellOperationTransition::Complete);
        REQUIRE(residency.Apply(authority, fresh.Operation()).Value().state == StreamingCellState::Resident);
    }

}  // namespace Horo::WorldStreaming
