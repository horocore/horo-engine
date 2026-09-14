#include "Horo/WorldStreaming/OriginRebaseTransaction.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <set>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        struct ParticipantLog final {
            std::vector<std::uint64_t> prepared;
            std::vector<std::uint64_t> applied;
            std::vector<std::uint64_t> rolledBack;

            ParticipantLog() {
                prepared.reserve(16);
                applied.reserve(16);
                rolledBack.reserve(16);
            }
        };

        class TestPreparedParticipant final : public IOriginRebasePreparedParticipant {
        public:
            TestPreparedParticipant(OriginRebasePreparedBinding binding, ParticipantLog &log) noexcept : binding_(binding), log_(log) {}

            OriginRebasePreparedBinding Binding() const noexcept override {
                return binding_;
            }

            void ApplyPrepared() noexcept override {
                if (applied_ || rolledBack_)
                    return;
                log_.applied.push_back(binding_.requirement.participant.Value());
                applied_ = true;
            }

            void RollbackPrepared() noexcept override {
                if (applied_ || rolledBack_)
                    return;
                log_.rolledBack.push_back(binding_.requirement.participant.Value());
                rolledBack_ = true;
            }

        private:
            OriginRebasePreparedBinding binding_{};
            ParticipantLog &log_;
            bool applied_{};
            bool rolledBack_{};
        };

        class TestParticipant final : public IOriginRebaseParticipant {
        public:
            TestParticipant(OriginRebaseParticipantRequirement requirement, ParticipantLog &log, const bool failPreparation = false,
                            const bool staleReceipt = false) noexcept
                : requirement_(requirement), log_(log), failPreparation_(failPreparation), staleReceipt_(staleReceipt) {}

            OriginRebaseParticipantRequirement Requirement() const noexcept override {
                return requirement_;
            }

            Result<std::unique_ptr<IOriginRebasePreparedParticipant>> Prepare(const OriginRebaseEvent &event) noexcept override {
                log_.prepared.push_back(requirement_.participant.Value());
                if (failPreparation_)
                    return Result<std::unique_ptr<IOriginRebasePreparedParticipant>>::Failure(
                        MakeError(WorldStreamingErrors::OriginRebaseStorageUnavailable));
                auto receiptRequirement = requirement_;
                if (staleReceipt_)
                    receiptRequirement.revision = IdentityFrom<OriginRebaseParticipantRevision>(99);
                OriginRebasePreparedBinding binding{
                    .requirement = receiptRequirement,
                    .transaction = event.transaction,
                    .previousFrame = event.previousFrame.Binding(),
                    .replacementFrame = event.replacementFrame.Binding(),
                };
                std::unique_ptr<IOriginRebasePreparedParticipant> receipt = std::make_unique<TestPreparedParticipant>(binding, log_);
                return Result<std::unique_ptr<IOriginRebasePreparedParticipant>>::Success(std::move(receipt));
            }

        private:
            OriginRebaseParticipantRequirement requirement_{};
            ParticipantLog &log_;
            bool failPreparation_{};
            bool staleReceipt_{};
        };

        [[nodiscard]] OriginFrameBinding FrameBinding(const std::uint64_t revision = 4, const std::uint64_t generation = 8) {
            return {.identity = IdentityFrom<OriginFrameId>(17),
                    .revision = IdentityFrom<OriginFrameRevision>(revision),
                    .generation = IdentityFrom<OriginGeneration>(generation)};
        }

        [[nodiscard]] OriginFrame Frame(const std::uint64_t revision = 4, const std::uint64_t generation = 8,
                                        const std::int64_t x = 10'000) {
            return OriginFrame::Create(FrameBinding(revision, generation), Math::WorldCoordinate64::FromMillimeters(x, -20'000, 30'000))
                .Value();
        }

        [[nodiscard]] OriginShiftDecision Decision(const OriginFrame &frame) {
            return {.request = IdentityFrom<OriginShiftRequestId>(91),
                    .observedFrame = frame.Binding(),
                    .requester = OriginShiftRequester::Gameplay,
                    .kind = OriginShiftDecisionKind::RequestShift,
                    .thresholdMillimeters = 1'000,
                    .targetOrigin = Math::WorldCoordinate64::FromMillimeters(12'000, -20'000, 30'000)};
        }

        [[nodiscard]] OriginRebaseTransactionContext Context(const OriginFrame &frame,
                                                             const OriginRebaseLifecycle lifecycle = OriginRebaseLifecycle::Active,
                                                             const std::size_t maximumParticipants = 4) {
            return {.transaction = IdentityFrom<OriginRebaseTransactionId>(61),
                    .decision = Decision(frame),
                    .activeFrame = frame,
                    .maximumParticipants = maximumParticipants,
                    .lifecycle = lifecycle};
        }

        [[nodiscard]] OriginRebaseParticipantRequirement Requirement(const std::uint64_t id, const std::uint64_t revision = 1) {
            return {.participant = IdentityFrom<OriginRebaseParticipantId>(id),
                    .revision = IdentityFrom<OriginRebaseParticipantRevision>(revision)};
        }

        [[nodiscard]] std::vector<IOriginRebaseParticipant *> ParticipantPointers(
            std::vector<std::unique_ptr<TestParticipant>> &participants) {
            std::vector<IOriginRebaseParticipant *> pointers;
            pointers.reserve(participants.size());
            for (const auto &participant : participants)
                pointers.push_back(participant.get());
            return pointers;
        }

        [[nodiscard]] std::vector<std::unique_ptr<TestParticipant>> Participants(
            const std::vector<OriginRebaseParticipantRequirement> &requirements, ParticipantLog &log) {
            std::vector<std::unique_ptr<TestParticipant>> participants;
            participants.reserve(requirements.size());
            for (const auto requirement : requirements)
                participants.push_back(std::make_unique<TestParticipant>(requirement, log));
            return participants;
        }
    }  // namespace

    TEST_CASE("Origin rebase gathers and atomically publishes every participant in canonical order",
              "[unit][world_streaming][origin_rebase]") {
        const auto activeFrame = Frame();
        auto owner = OriginFrameOwner::Create(activeFrame).Value();
        auto oldLease = owner->Lease().Value();
        const std::vector required{Requirement(30), Requirement(10), Requirement(20)};
        ParticipantLog log;
        auto participants = Participants(required, log);
        auto pointers = ParticipantPointers(participants);
        auto transaction = OriginRebaseTransaction::Prepare(Context(activeFrame), required, pointers).Value();

        REQUIRE(transaction.State() == OriginRebaseTransactionState::Prepared);
        REQUIRE(transaction.Requirements()[0] == Requirement(10));
        REQUIRE(transaction.Event().previousFrame == activeFrame);
        REQUIRE(transaction.Event().replacementFrame.Binding().revision == IdentityFrom<OriginFrameRevision>(5));
        REQUIRE(transaction.Event().replacementFrame.Binding().generation == IdentityFrom<OriginGeneration>(9));
        REQUIRE(log.prepared == std::vector<std::uint64_t>{10, 20, 30});

        REQUIRE(transaction.Commit(*owner, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Active).HasValue());
        REQUIRE(transaction.State() == OriginRebaseTransactionState::Published);
        REQUIRE(log.applied == std::vector<std::uint64_t>{10, 20, 30});
        REQUIRE(log.rolledBack.empty());
        REQUIRE(oldLease.Get().HasError());
        const auto published = owner->Lease().Value().Get().Value();
        REQUIRE(published == transaction.Event().replacementFrame);
        RequireError(transaction.Commit(*owner, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Active),
                     WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
    }

    TEST_CASE("Origin rebase retains preparation until the declared host safe point",
              "[unit][world_streaming][origin_rebase][safe_point]") {
        const auto activeFrame = Frame();
        auto owner = OriginFrameOwner::Create(activeFrame).Value();
        const std::vector required{Requirement(10)};
        ParticipantLog log;
        auto participants = Participants(required, log);
        auto pointers = ParticipantPointers(participants);
        auto transaction = OriginRebaseTransaction::Prepare(Context(activeFrame), required, pointers).Value();

        RequireError(transaction.Commit(*owner, OriginRebaseCommitPoint::PreSimulation, OriginRebaseLifecycle::Active),
                     WorldStreamingErrors::OriginRebaseSafePointUnavailable);
        REQUIRE(transaction.State() == OriginRebaseTransactionState::Prepared);
        REQUIRE(log.applied.empty());
        REQUIRE(log.rolledBack.empty());
        REQUIRE(owner->Lease().Value().Get().Value() == activeFrame);
    }

    TEST_CASE("Origin rebase rolls back acquired readiness when any required participant fails",
              "[unit][world_streaming][origin_rebase][rollback]") {
        const auto activeFrame = Frame();
        const std::vector required{Requirement(10), Requirement(20), Requirement(30)};
        ParticipantLog log;
        std::vector<std::unique_ptr<TestParticipant>> participants;
        participants.push_back(std::make_unique<TestParticipant>(required[0], log));
        participants.push_back(std::make_unique<TestParticipant>(required[1], log, true));
        participants.push_back(std::make_unique<TestParticipant>(required[2], log));
        auto pointers = ParticipantPointers(participants);

        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), required, pointers),
                     WorldStreamingErrors::OriginRebaseStorageUnavailable);
        REQUIRE(log.prepared == std::vector<std::uint64_t>{10, 20});
        REQUIRE(log.applied.empty());
        REQUIRE(log.rolledBack == std::vector<std::uint64_t>{10});
    }

    TEST_CASE("Origin rebase rejects incomplete duplicate over-capacity and stale participant sets before publication",
              "[unit][world_streaming][origin_rebase][failure]") {
        const auto activeFrame = Frame();
        const std::vector required{Requirement(10), Requirement(20)};
        ParticipantLog log;
        auto participants = Participants(required, log);
        auto pointers = ParticipantPointers(participants);

        const std::vector<OriginRebaseParticipantRequirement> empty;
        const std::vector<IOriginRebaseParticipant *> noParticipants;
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), empty, noParticipants),
                     WorldStreamingErrors::OriginRebaseIncomplete);
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame, OriginRebaseLifecycle::Active, 1), required, pointers),
                     WorldStreamingErrors::OriginRebaseCapacityExceeded);

        const std::vector duplicated{Requirement(10), Requirement(10)};
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), duplicated, pointers),
                     WorldStreamingErrors::OriginRebaseInvalid);

        ParticipantLog duplicateLog;
        std::vector<std::unique_ptr<TestParticipant>> duplicateParticipants;
        duplicateParticipants.push_back(std::make_unique<TestParticipant>(Requirement(10), duplicateLog));
        duplicateParticipants.push_back(std::make_unique<TestParticipant>(Requirement(10), duplicateLog));
        auto duplicatePointers = ParticipantPointers(duplicateParticipants);
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), required, duplicatePointers),
                     WorldStreamingErrors::OriginRebaseInvalid);

        const std::vector<IOriginRebaseParticipant *> missingPointers{pointers.front()};
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), required, missingPointers),
                     WorldStreamingErrors::OriginRebaseIncomplete);

        const std::vector<IOriginRebaseParticipant *> nullPointers{pointers.front(), nullptr};
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), required, nullPointers),
                     WorldStreamingErrors::OriginRebaseInvalid);

        ParticipantLog staleLog;
        std::vector<std::unique_ptr<TestParticipant>> staleParticipants;
        staleParticipants.push_back(std::make_unique<TestParticipant>(required[0], staleLog));
        staleParticipants.push_back(std::make_unique<TestParticipant>(required[1], staleLog, false, true));
        auto stalePointers = ParticipantPointers(staleParticipants);
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame), required, stalePointers),
                     WorldStreamingErrors::OriginRebaseStale);
        REQUIRE(staleLog.rolledBack == std::vector<std::uint64_t>{20, 10});
    }

    TEST_CASE("Origin rebase validates authorized decision evidence before participant callbacks",
              "[unit][world_streaming][origin_rebase][validation]") {
        const auto activeFrame = Frame();
        const std::vector required{Requirement(10)};
        ParticipantLog log;
        auto participants = Participants(required, log);
        auto pointers = ParticipantPointers(participants);

        auto invalid = Context(activeFrame);
        invalid.transaction = {};
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseInvalid);
        invalid = Context(activeFrame);
        invalid.decision.kind = OriginShiftDecisionKind::Remain;
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseInvalid);
        invalid = Context(activeFrame);
        invalid.decision.requester = OriginShiftRequester::Count;
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseInvalid);
        invalid = Context(activeFrame);
        invalid.decision.thresholdMillimeters = 0;
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseInvalid);
        invalid = Context(activeFrame);
        invalid.decision.targetOrigin = activeFrame.Origin();
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseInvalid);
        invalid = Context(activeFrame);
        invalid.decision.observedFrame.revision = IdentityFrom<OriginFrameRevision>(5);
        RequireError(OriginRebaseTransaction::Prepare(invalid, required, pointers), WorldStreamingErrors::OriginRebaseStale);
        REQUIRE(log.prepared.empty());
    }

    TEST_CASE("Origin rebase fences replacement cancellation shutdown and exhausted generations",
              "[unit][world_streaming][origin_rebase][lifecycle]") {
        const auto activeFrame = Frame();
        const std::vector required{Requirement(10)};

        ParticipantLog cancelledLog;
        auto cancelledParticipants = Participants(required, cancelledLog);
        auto cancelledPointers = ParticipantPointers(cancelledParticipants);
        RequireError(OriginRebaseTransaction::Prepare(Context(activeFrame, OriginRebaseLifecycle::Cancelling), required, cancelledPointers),
                     WorldStreamingErrors::OriginRebaseLifecycleUnavailable);

        ParticipantLog staleLog;
        auto staleParticipants = Participants(required, staleLog);
        auto stalePointers = ParticipantPointers(staleParticipants);
        auto stale = OriginRebaseTransaction::Prepare(Context(activeFrame), required, stalePointers).Value();
        auto replacementOwner = OriginFrameOwner::Create(Frame(5, 9, 11'000)).Value();
        RequireError(stale.Commit(*replacementOwner, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Active),
                     WorldStreamingErrors::OriginRebaseStale);
        REQUIRE(staleLog.rolledBack == std::vector<std::uint64_t>{10});

        ParticipantLog closedLog;
        auto closedParticipants = Participants(required, closedLog);
        auto closedPointers = ParticipantPointers(closedParticipants);
        auto closed = OriginRebaseTransaction::Prepare(Context(activeFrame), required, closedPointers).Value();
        auto owner = OriginFrameOwner::Create(activeFrame).Value();
        RequireError(closed.Commit(*owner, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Closed),
                     WorldStreamingErrors::OriginRebaseLifecycleUnavailable);
        REQUIRE(closedLog.rolledBack == std::vector<std::uint64_t>{10});
        REQUIRE(owner->Lease().Value().Get().Value() == activeFrame);

        ParticipantLog unsupportedLog;
        auto unsupportedParticipants = Participants(required, unsupportedLog);
        auto unsupportedPointers = ParticipantPointers(unsupportedParticipants);
        auto unsupported = OriginRebaseTransaction::Prepare(Context(activeFrame), required, unsupportedPointers).Value();
        RequireError(unsupported.Commit(*owner, OriginRebaseCommitPoint::Count, OriginRebaseLifecycle::Active),
                     WorldStreamingErrors::OriginRebaseUnsupported);
        REQUIRE(unsupported.State() == OriginRebaseTransactionState::Prepared);
        RequireError(unsupported.Commit(*owner, OriginRebaseCommitPoint::PostSimulation, OriginRebaseLifecycle::Count),
                     WorldStreamingErrors::OriginRebaseUnsupported);
        REQUIRE(unsupported.State() == OriginRebaseTransactionState::RolledBack);
        REQUIRE(unsupportedLog.rolledBack == std::vector<std::uint64_t>{10});

        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto exhaustedFrame = Frame(maximum, maximum);
        ParticipantLog exhaustedLog;
        auto exhaustedParticipants = Participants(required, exhaustedLog);
        auto exhaustedPointers = ParticipantPointers(exhaustedParticipants);
        RequireError(OriginRebaseTransaction::Prepare(Context(exhaustedFrame), required, exhaustedPointers),
                     WorldStreamingErrors::GenerationExhausted);
        REQUIRE(exhaustedLog.prepared.empty());
    }

    TEST_CASE("Origin rebase errors expose unique stable descriptors", "[unit][world_streaming][origin_rebase][errors]") {
        CHECK(WorldStreamingErrors::OriginRebaseInvalid.domain.Value() == std::string_view{"horo.world_streaming"});
        const std::set<std::string_view> codes{
            WorldStreamingErrors::OriginRebaseInvalid.code.Value(),
            WorldStreamingErrors::OriginRebaseUnsupported.code.Value(),
            WorldStreamingErrors::OriginRebaseStale.code.Value(),
            WorldStreamingErrors::OriginRebaseIncomplete.code.Value(),
            WorldStreamingErrors::OriginRebaseCapacityExceeded.code.Value(),
            WorldStreamingErrors::OriginRebaseLifecycleUnavailable.code.Value(),
            WorldStreamingErrors::OriginRebaseSafePointUnavailable.code.Value(),
            WorldStreamingErrors::OriginRebaseStorageUnavailable.code.Value(),
        };
        CHECK(codes.size() == 8);
    }
}  // namespace Horo::WorldStreaming
