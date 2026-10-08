#pragma once

#include "Horo/WorldStreaming/StreamingCellDirection.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace Horo::WorldStreaming::DirectionTestSupport {
    using TestSupport::IdentityFrom;
    using TestSupport::RequireError;
    using Participants = std::vector<std::unique_ptr<IStreamingCellRetirementParticipant>>;

    inline StreamingCellDemandRevision Revision(const std::uint64_t revision) {
        return IdentityFrom<StreamingCellDemandRevision>(revision);
    }

    inline StreamingCellActivationRequirement Requirement(const std::uint64_t id, const std::uint64_t revision = 1) {
        return {IdentityFrom<StreamingRuntimeServiceId>(id), IdentityFrom<StreamingRuntimeServiceRevision>(revision)};
    }

    inline StreamingCellDirectionConfig Config(const StreamingCellOperationKind kind = StreamingCellOperationKind::Load,
                                               const std::uint64_t generation = 1) {
        auto handle = CandidateTestSupport::Operation(IdentityFrom<StreamingGeneration>(generation));
        handle.operation = IdentityFrom<StreamingCellOperationId>(generation);
        return {StreamingCellOperation::Create(handle, kind).Value(), Revision(1), StreamingDesiredResidency::Activated, 4, 5};
    }

    inline StreamingSchedulerAdmissionLedger Scheduler(const std::uint32_t operations = 1, const std::uint64_t capacity = 5) {
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
        Participant(const StreamingCellActivationRequirement requirement, const StreamingCellOperationHandle operation, ParticipantLog &log)
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
                StreamingCellRetirementAcknowledgement{staleRevision ? Horo::WorldStreaming::DirectionTestSupport::Requirement(1, 2)
                                                                     : requirement_,
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

    inline Participant &Add(Participants &participants, ParticipantLog &log, const StreamingCellDirectionConfig &config,
                            const std::uint64_t id) {
        auto participant = std::make_unique<Participant>(Requirement(id), config.operation.Handle(), log);
        auto &view = *participant;
        participants.push_back(std::move(participant));
        return view;
    }

    inline void Forward(StreamingCellDirectionOwner &owner, const StreamingCellOperationTransition transition) {
        REQUIRE(owner.Advance(owner.Operation(), transition).HasValue());
    }

    inline void Demand(StreamingCellDirectionOwner &owner, const StreamingDesiredResidency desired) {
        REQUIRE(
            owner.UpdateDemand(owner.Operation().Handle(), owner.Revision(), Revision(owner.Revision().Value() + 1), desired).HasValue());
    }

    class ActivationReceipt final : public IStreamingCellActivationReceipt {
    public:
        ActivationReceipt(const StreamingCellOperationHandle operation, ParticipantLog &log) : operation_(operation), log_(log) {}

        StreamingCellActivationRequirement Requirement() const noexcept override {
            return Horo::WorldStreaming::DirectionTestSupport::Requirement(1);
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

    inline StreamingCellActivationTransaction Prepared(StreamingCellDirectionOwner &owner, ParticipantLog &log) {
        std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts;
        receipts.push_back(std::make_unique<ActivationReceipt>(owner.Operation().Handle(), log));
        const std::array required{Requirement(1)};
        return StreamingCellActivationTransaction::Prepare({IdentityFrom<StreamingCellActivationId>(1), owner.Operation(), 1,
                                                            StreamingCellActivationLifecycle::Active,
                                                            IdentityFrom<StreamingSchedulerLedgerId>(1)},
                                                           required, std::move(receipts))
            .Value();
    }
}  // namespace Horo::WorldStreaming::DirectionTestSupport
