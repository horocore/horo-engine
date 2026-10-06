#include "Horo/WorldStreaming/StreamingCellDirection.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;
        using Participants = std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>>;

        StreamingCellDemandRevision Revision(const std::uint64_t revision) {
            return IdentityFrom<StreamingCellDemandRevision>(revision);
        }

        StreamingCellActivationRequirement Requirement(const std::uint64_t id, const std::uint64_t revision = 1) {
            return {IdentityFrom<StreamingRuntimeServiceId>(id), IdentityFrom<StreamingRuntimeServiceRevision>(revision)};
        }

        StreamingCellDirectionConfig Config(const StreamingCellOperationKind kind = StreamingCellOperationKind::Load,
                                            const std::uint64_t generation = 1) {
            auto handle = CandidateTestSupport::Operation(IdentityFrom<StreamingGeneration>(generation));
            handle.operation = IdentityFrom<StreamingCellOperationId>(generation);
            return {StreamingCellOperation::Create(handle, kind).Value(), Revision(1), StreamingDesiredResidency::Activated, 4, 5};
        }

        StreamingSchedulerAdmissionLedger Scheduler(const std::uint32_t operations = 1, const std::uint64_t capacity = 5) {
            return StreamingSchedulerAdmissionLedger::Create(IdentityFrom<StreamingSchedulerLedgerId>(1),
                                                             {operations,
                                                              capacity,
                                                              {WorldPartitionProjectProfile::Editor,
                                                               IdentityFrom<StreamingConcurrencyRevision>(1), operations, operations,
                                                               operations}})
                .Value();
        }

        struct ParticipantLog final {
            std::vector<std::uint64_t> revoked;
            std::vector<std::uint64_t> started;
            std::vector<std::uint64_t> destroyed;
            std::size_t leases{};
            bool published{};
            bool rolledBack{};
        };

        class Participant final : public IStreamingCellRetirementParticipant {
        public:
            Participant(const StreamingCellActivationRequirement requirement, const StreamingCellOperationHandle operation,
                        ParticipantLog &log)
                : requirement_(requirement), operation_(operation), log_(log) {
                ++log_.leases;
            }

            ~Participant() override {
                log_.destroyed.push_back(requirement_.participant.Value());
                if (!leaseRetired_)
                    --log_.leases;
            }

            StreamingCellActivationRequirement Requirement() const noexcept override {
                return requirement_;
            }

            StreamingCellOperationHandle Operation() const noexcept override {
                return operation_;
            }

            std::uint64_t MaximumRetirementNanoseconds() const noexcept override {
                return cost;
            }

            std::uint64_t cost{10};

            void RevokeAccess() noexcept override {
                log_.revoked.push_back(requirement_.participant.Value());
            }

            void BeginRetirement() noexcept override {
                log_.started.push_back(requirement_.participant.Value());
            }

            Result<std::optional<StreamingCellRetirementAcknowledgement>> PollRetirement() override {
                if (failPoll)
                    return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Failure(
                        MakeError(WorldStreamingErrors::CellAssetRequestUnavailable));
                if (!ready)
                    return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Success(std::nullopt);
                if (!staleRevision && !staleFence && !leaseRetired_) {
                    --log_.leases;
                    leaseRetired_ = true;
                }
                return Result<std::optional<StreamingCellRetirementAcknowledgement>>::Success(
                    StreamingCellRetirementAcknowledgement{staleRevision ? Horo::WorldStreaming::Requirement(1, 2) : requirement_,
                                                           staleFence ? Config(StreamingCellOperationKind::Load, 2).operation.Handle()
                                                                      : operation_});
            }

            bool ready{};
            bool failPoll{};
            bool staleRevision{};
            bool staleFence{};

        private:
            StreamingCellActivationRequirement requirement_;
            StreamingCellOperationHandle operation_;
            ParticipantLog &log_;
            bool leaseRetired_{};
        };

        Participant &Add(Participants &participants, ParticipantLog &log, const StreamingCellDirectionConfig &config,
                         const std::uint64_t id) {
            auto participant = std::make_unique<Participant>(Requirement(id), config.operation.Handle(), log);
            auto &view = *participant;
            participants.push_back(std::move(participant));
            return view;
        }

        void Forward(StreamingCellDirectionOwner &owner, const StreamingCellOperationTransition transition) {
            REQUIRE(owner.Advance(owner.Operation(), transition).HasValue());
        }

        void Demand(StreamingCellDirectionOwner &owner, const StreamingDesiredResidency desired) {
            REQUIRE(owner.UpdateDemand(owner.Operation().Handle(), owner.Revision(), Revision(owner.Revision().Value() + 1), desired)
                        .HasValue());
        }

        class ActivationReceipt final : public IStreamingCellActivationReceipt {
        public:
            ActivationReceipt(const StreamingCellOperationHandle operation, ParticipantLog &log) : operation_(operation), log_(log) {}

            StreamingCellActivationRequirement Requirement() const noexcept override {
                return Horo::WorldStreaming::Requirement(1);
            }

            StreamingCellOperationHandle Operation() const noexcept override {
                return operation_;
            }

            std::uint64_t MaximumPublicationNanoseconds() const noexcept override {
                return 10;
            }

            void PublishPrepared() noexcept override {
                log_.published = true;
            }

            void RollbackPrepared() noexcept override {
                if (!log_.published)
                    log_.rolledBack = true;
            }

        private:
            StreamingCellOperationHandle operation_;
            ParticipantLog &log_;
        };

        StreamingCellActivationTransaction Prepared(StreamingCellDirectionOwner &owner, ParticipantLog &log) {
            std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts;
            receipts.push_back(std::make_unique<ActivationReceipt>(owner.Operation().Handle(), log));
            const std::array required{Requirement(1)};
            return StreamingCellActivationTransaction::Prepare({IdentityFrom<StreamingCellActivationId>(1), owner.Operation(), 1,
                                                                StreamingCellActivationLifecycle::Active,
                                                                IdentityFrom<StreamingSchedulerLedgerId>(1)},
                                                               required, std::move(receipts))
                .Value();
        }
    }  // namespace

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
