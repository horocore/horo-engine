#include "StreamingDirectionFinalizationTestSupport.h"

#include <functional>

namespace Horo::WorldStreaming::Detail {
    /** @brief Test-only forward exhaustion; cannot reset revisions or access child authorities. */
    class StreamingFeatureBudgetRevisionTestAccess final {
    public:
        static void LeavePublications(StreamingFeatureBudgetReservations &ledger, const std::uint64_t remaining) {
            const auto next = std::numeric_limits<std::uint64_t>::max() - remaining;
            REQUIRE(next >= ledger.revision_.Value());
            ledger.revision_ = StreamingFeatureBudgetRevision::Create(next).Value();
        }
    };
}

namespace Horo::WorldStreaming {
    namespace {
        using namespace FinalizationTestSupport;
        using TestSupport::RequireError;
        using Detail::StreamingFeatureBudgetRevisionTestAccess;
        using enum StreamingCellOperationTransition;

        struct Fixture final {
            Host host;
            StreamingFeatureBudgetReservations ledger{host.Ledger(1)};
            StreamingCellDirectionConfig config;
            DirectionTestSupport::ParticipantLog log;
            DirectionTestSupport::Participants participants;
            DirectionTestSupport::Participant *participant{};
            StreamingCellDirectionOwner owner;

            explicit Fixture(const StreamingCellOperationKind kind = StreamingCellOperationKind::Load)
                : config(DirectionTestSupport::Config(kind)), owner(Create()) {}

            StreamingCellDirectionOwner Create() {
                participant = &DirectionTestSupport::Add(participants, log, config, 1);
                return StreamingCellDirectionOwner::Create(ledger, config, Plan(Feature::Terrain, Amounts(12)),
                                                            config.operation.Handle().fence, participants).Value();
            }

            StreamingOwnerFrameBudget Frame() const {
                return StreamingOwnerFrameBudget::Create({ledger.Runtime().Scheduler().Owner(),
                    IdentityFrom<StreamingOwnerWorkRevision>(1), IdentityFrom<StreamingOwnerFrameId>(1), 10000, 1000}).Value();
            }

            void Activating() {
                DirectionTestSupport::Forward(owner, BeginPreparation);
                DirectionTestSupport::Forward(owner, BeginActivation);
            }
        };

        class Receipt final : public IStreamingCellActivationReceipt {
        public:
            explicit Receipt(Fixture &fixture, std::function<void()> callback = {}) : fixture_(fixture), callback_(std::move(callback)) {}
            StreamingCellActivationRequirement Requirement() const noexcept override {
                return DirectionTestSupport::Requirement(1);
            }
            StreamingCellOperationHandle Operation() const noexcept override { return fixture_.owner.Operation().Handle(); }
            std::uint64_t MaximumPublicationNanoseconds() const noexcept override { return 10; }
            void PublishPrepared() noexcept override {
                fixture_.log.published = true;
                if (callback_) callback_();
            }
            void RollbackPrepared() noexcept override { fixture_.log.rolledBack = true; }
        private:
            Fixture &fixture_;
            std::function<void()> callback_;
        };

        StreamingCellActivationTransaction Prepare(Fixture &fixture, std::function<void()> callback = {}) {
            std::vector<std::unique_ptr<IStreamingCellActivationReceipt>> receipts;
            receipts.push_back(std::make_unique<Receipt>(fixture, std::move(callback)));
            const std::array required{DirectionTestSupport::Requirement(1)};
            return StreamingCellActivationTransaction::Prepare({IdentityFrom<StreamingCellActivationId>(1), fixture.owner.Operation(), 1,
                StreamingCellActivationLifecycle::Active, fixture.ledger.Runtime().Scheduler().Owner()}, required, std::move(receipts)).Value();
        }

        TEST_CASE("Aggregate finalization releases load at the last publication", "[unit][world_streaming][fix1]") {
            Fixture fixture;
            DirectionTestSupport::Forward(fixture.owner, BeginPreparation);
            StreamingFeatureBudgetRevisionTestAccess::LeavePublications(fixture.ledger, 1);
            const auto completed = fixture.owner.Advance(fixture.owner.Operation(), Complete);
            CHECK(completed.HasValue());
            CHECK(fixture.ledger.Context().revision.Value() == std::numeric_limits<std::uint64_t>::max());
            CHECK(fixture.ledger.Runtime().Scheduler().ReservedCount() == 0);
            CHECK(fixture.ledger.Runtime().Scheduler().ReservedCapacityUnits() == 0);
            CHECK(fixture.ledger.Used(Axis::CpuResidentBytes).Value() == 0);
            CHECK(fixture.owner.TakeTerminalResult().HasValue());
            CHECK(fixture.owner.TakeSucceededParticipants().HasValue());
            RequireError(fixture.ledger.BeginShutdown(fixture.ledger.Context()), WorldStreamingErrors::GenerationExhausted);
        }

        TEST_CASE("Aggregate finalization releases exact retirement at the last publication", "[unit][world_streaming][fix1]") {
            Fixture fixture;
            REQUIRE(fixture.owner.Interrupt(fixture.owner.Operation().Handle(), Cancel).HasValue());
            fixture.participant->ready = true;
            auto frame = fixture.Frame();
            StreamingFeatureBudgetRevisionTestAccess::LeavePublications(fixture.ledger, 1);
            const auto retired = fixture.owner.PollRetirement(frame, 0);
            CHECK(retired.HasValue());
            CHECK(fixture.ledger.Context().revision.Value() == std::numeric_limits<std::uint64_t>::max());
            CHECK(fixture.ledger.Runtime().Scheduler().ReservedCount() == 0);
            CHECK(fixture.ledger.Used(Axis::CpuResidentBytes).Value() == 0);
            CHECK(fixture.log.leases == 0);
            CHECK(fixture.owner.TakeTerminalResult().HasValue());
        }

        TEST_CASE("Aggregate activation rolls back before publication when exhausted", "[unit][world_streaming][fix1]") {
            Fixture fixture{StreamingCellOperationKind::Activate};
            fixture.Activating();
            auto transaction = Prepare(fixture);
            auto frame = fixture.Frame();
            StreamingFeatureBudgetRevisionTestAccess::LeavePublications(fixture.ledger, 0);
            RequireError(fixture.owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                frame, 0), WorldStreamingErrors::GenerationExhausted);
            CHECK_FALSE(fixture.log.published);
            CHECK(fixture.log.rolledBack);
            CHECK(transaction.State() == StreamingCellActivationState::RolledBack);
            CHECK(fixture.owner.Operation().State() == StreamingCellOperationState::Activating);
            CHECK(fixture.ledger.Runtime().Scheduler().ReservedCount() == 1);
            CHECK(fixture.ledger.Used(Axis::CpuResidentBytes).Value() == 12);
            CHECK(fixture.ledger.Context().revision.Value() == std::numeric_limits<std::uint64_t>::max());
        }

        TEST_CASE("Aggregate activation finalization excludes callback reentry at last publication", "[unit][world_streaming][fix1]") {
            Fixture fixture{StreamingCellOperationKind::Activate};
            fixture.Activating();
            bool shutdownRejected{}, interruptionRejected{};
            auto transaction = Prepare(fixture, [&] {
                shutdownRejected = fixture.ledger.BeginShutdown(fixture.ledger.Context()).HasError();
                interruptionRejected = fixture.owner.Interrupt(fixture.owner.Operation().Handle(), Cancel).HasError();
            });
            auto frame = fixture.Frame();
            StreamingFeatureBudgetRevisionTestAccess::LeavePublications(fixture.ledger, 1);
            CHECK(fixture.owner.CommitActivation(transaction, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges,
                frame, 0).HasValue());
            CHECK(shutdownRejected);
            CHECK(interruptionRejected);
            CHECK(fixture.log.published);
            CHECK_FALSE(fixture.log.rolledBack);
            CHECK(fixture.ledger.Runtime().Scheduler().ReservedCount() == 0);
            CHECK(fixture.ledger.Used(Axis::CpuResidentBytes).Value() == 0);
            CHECK(fixture.ledger.Context().revision.Value() == std::numeric_limits<std::uint64_t>::max());
            CHECK(fixture.owner.TakeTerminalResult().HasValue());
            CHECK(fixture.owner.TakeSucceededParticipants().HasValue());
        }
    }
}
