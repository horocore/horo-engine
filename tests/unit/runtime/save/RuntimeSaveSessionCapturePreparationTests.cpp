#include "Horo/Runtime/Save/RuntimeSaveSession.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace CaptureTestSupport;

        template <class T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        class PreparationClock final : public Clock {
        public:
            Duration MonotonicNow() const override {
                return now;
            }

            Duration now{};
        };

        class PreparationWriter final : public IRuntimeSaveWriter {
        public:
            Result<void> Prepare(const RuntimeSaveSnapshot &snapshot, const CancellationToken &) override {
                actual = snapshot;
                return Result<void>::Success();
            }

            Result<void> Commit() override {
                ++commits;
                return Result<void>::Success();
            }

            RuntimeSaveSnapshot actual;
            unsigned commits{};
        };

        class OwnerPreparation final : public IRuntimeSaveCapturePreparation {
        public:
            using Callback = std::function<Result<RuntimeSaveCapturePreparationEvidence>(const RuntimeSaveCapturePreparationContext &)>;

            explicit OwnerPreparation(Callback callback) : callback_(std::move(callback)) {}

            Result<RuntimeSaveCapturePreparationEvidence> Prepare(const RuntimeSaveCapturePreparationContext &context) override {
                ++calls;
                observedThread = std::this_thread::get_id();
                return callback_(context);
            }

            unsigned calls{};
            std::thread::id observedThread;

        private:
            Callback callback_;
        };

        /** @brief Real session/barrier/jobs fixture; bytes model protocol keys, not native Physics qualification. */
        struct PreparationFixture final {
            PreparationClock clock;
            JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
            CanonicalStateParticipantRegistry registry;
            std::unique_ptr<SaveCaptureBarrier> barrier;
            SaveParticipantRegistrySnapshot participants;
            RuntimeSaveCaptureProvenance provenance;
            SaveRuntimeGeneration generation;
            std::unique_ptr<RuntimeSaveSession> session;
            std::shared_ptr<PreparationWriter> writer = std::make_shared<PreparationWriter>();
            std::array<std::size_t, 5> owners{};
            std::array<std::byte, 4> currentKeys{std::byte{1}};
            std::array<std::byte, 4> preparedKeys{};
            unsigned captures{};
            bool requiresPreparation{true};
            const std::thread::id owner{std::this_thread::get_id()};

            PreparationFixture() {
                barrier = Take(SaveCaptureBarrier::Create(1391, clock, 8));
                RegisterOwners();
                RegisterCapture();
                participants = Take(registry.Snapshot());
                provenance = Provenance(participants);
                generation = {1, provenance.sceneIncarnation, participants.Generation()};
                session = Take(RuntimeSaveSession::Create(jobs, *barrier, participants, generation));
            }

            ~PreparationFixture() {
                session.reset();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }

            void RegisterOwners() {
                const std::array names{"host.fixed", "host.jobs", "host.scene", "host.subsystem", "game.keys"};
                const std::array domains{SaveBarrierDomain::FixedSimulation, SaveBarrierDomain::Jobs, SaveBarrierDomain::SceneMutation,
                                         SaveBarrierDomain::Subsystem, SaveBarrierDomain::Subsystem};
                for (std::size_t i = 0; i < owners.size(); ++i) {
                    owners[i] = Take(barrier->Register(Participant(names[i]), domains[i]));
                    REQUIRE(barrier->PublishReadiness(owners[i], {41}).HasValue());
                }
            }

            void RegisterCapture() {
                const auto record = Test::Id<SaveRecordId>(1);
                Register(registry, Descriptor("game.keys", {record}),
                         std::make_shared<
                             CallbackCaptureAdapter>([this, record](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                    using Outcome = Result<CanonicalCaptureDisposition>;
                    if (std::this_thread::get_id() != owner)
                        return Outcome::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
                    if (requiresPreparation && preparedKeys != currentKeys)
                        return Outcome::Failure(MakeError(SaveErrors::CaptureRegistryStale));
                    ++captures;
                    if (auto copied = sink.WriteCopied(record, currentKeys); copied.HasError())
                        return Outcome::Failure(copied.ErrorValue());
                    return Outcome::Success(CanonicalCaptureDisposition::Captured);
                }, std::make_shared<int>()));
            }

            RuntimeSaveCapturePreparationEvidence Evidence() const {
                return {generation, provenance};
            }

            std::shared_ptr<OwnerPreparation> Preparation() {
                return std::make_shared<OwnerPreparation>([this](const RuntimeSaveCapturePreparationContext &) {
                    preparedKeys = currentKeys;
                    return Result<RuntimeSaveCapturePreparationEvidence>::Success(Evidence());
                });
            }

            RuntimeSaveRequest Save(std::shared_ptr<IRuntimeSaveCapturePreparation> preparation) {
                return Take(session->Save({.operation = 1, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 1}, writer,
                                          std::move(preparation)));
            }

            Result<void> Pump() {
                return session->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, provenance);
            }

            void Finish(const RuntimeSaveRequest &request) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while ((!request.Operation().Snapshot()->IsTerminal() || !session->IsSettled()) &&
                       std::chrono::steady_clock::now() < deadline) {
                    const auto result = Pump();
                    if (result.HasError())
                        INFO(result.ErrorValue().message);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                REQUIRE(request.Operation().Snapshot()->IsTerminal());
                REQUIRE(session->IsSettled());
            }

            void MutateKeys() {
                const auto mutation = Take(barrier->BeginMutation(owners[2]));
                currentKeys[1] = std::byte{2};
                ++provenance.sceneRevision;
                ++provenance.epoch.value;
                REQUIRE(barrier->EndMutation(mutation, provenance.epoch).HasValue());
                for (auto id : owners)
                    REQUIRE(barrier->PublishReadiness(id, provenance.epoch).HasValue());
            }
        };

        TEST_CASE("Queued Save prepares current owner keys after an intervening structural mutation", "[save][session][preparation]") {
            PreparationFixture fixture;
            auto preparation = fixture.Preparation();
            const auto request = fixture.Save(preparation);
            CHECK(preparation->calls == 0);
            fixture.MutateKeys();
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Completed);
            CHECK(preparation->calls == 1);
            CHECK(preparation->observedThread == fixture.owner);
            CHECK(fixture.captures == 1);
            REQUIRE(fixture.writer->actual.Records().size() == 1);
            const auto bytes = fixture.writer->actual.Records()[0].Segment(0);
            REQUIRE(bytes.size() == fixture.currentKeys.size());
            CHECK(std::ranges::equal(bytes, fixture.currentKeys));
            REQUIRE(fixture.Pump().HasValue());
            CHECK(preparation->calls == 1);
        }

        TEST_CASE("Owner preparation runs once per pending provenance and never after capture dispatch", "[save][session][preparation]") {
            PreparationFixture fixture;
            const auto mutation = Take(fixture.barrier->BeginMutation(fixture.owners[2]));
            auto preparation = fixture.Preparation();
            const auto request = fixture.Save(preparation);
            REQUIRE(fixture.Pump().HasValue());
            REQUIRE(fixture.Pump().HasValue());
            CHECK(preparation->calls == 1);
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.barrier->EndMutation(mutation, fixture.provenance.epoch).HasValue());
            fixture.Finish(request);
            CHECK(preparation->calls == 1);
            CHECK(fixture.captures == 1);
        }

        TEST_CASE("Unready owner evidence retains the same Save and is never cached or dispatched",
                  "[save][session][preparation][deferred]") {
            PreparationFixture fixture;
            bool ready{};
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &) {
                auto evidence = fixture.Evidence();
                evidence.ready = ready;
                if (ready)
                    fixture.preparedKeys = fixture.currentKeys;
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(evidence);
            });
            const auto request = fixture.Save(preparation);
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                REQUIRE(fixture.Pump().HasValue());
                CHECK_FALSE(request.Operation().Snapshot()->IsTerminal());
                CHECK(fixture.captures == 0);
                CHECK(fixture.writer->commits == 0);
            }
            CHECK(preparation->calls == 3);
            ready = true;
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Completed);
            CHECK(preparation->calls == 4);
            CHECK(fixture.captures == 1);
            CHECK(fixture.writer->commits == 1);
        }

        TEST_CASE("Actual quiescence deadline begins only after selected owner preparation is ready",
                  "[save][session][preparation][deferred][deadline]") {
            PreparationFixture fixture;
            bool ready{};
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &) {
                auto evidence = fixture.Evidence();
                evidence.ready = ready;
                if (ready)
                    fixture.preparedKeys = fixture.currentKeys;
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(evidence);
            });
            const auto request = fixture.Save(preparation);
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Idle);
            fixture.clock.now = Duration::FromMilliseconds(1000);
            REQUIRE(fixture.Pump().HasValue());
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Idle);
            CHECK_FALSE(request.Operation().Snapshot()->IsTerminal());
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
            ready = true;
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Completed);
            CHECK(fixture.captures == 1);
            CHECK(fixture.writer->commits == 1);
        }

        TEST_CASE("Started preparation barrier retains its original quiescence timeout across repeated owner pumps",
                  "[save][session][preparation][deadline]") {
            PreparationFixture fixture;
            const auto mutation = Take(fixture.barrier->BeginMutation(fixture.owners[2]));
            const auto request = fixture.Save(fixture.Preparation());
            REQUIRE(fixture.Pump().HasValue());
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Pending);
            fixture.clock.now = Duration::FromMilliseconds(50);
            REQUIRE(fixture.Pump().HasValue());
            CHECK_FALSE(request.Operation().Snapshot()->IsTerminal());
            fixture.clock.now = Duration::FromMilliseconds(100);
            const auto timedOut = fixture.Pump();
            REQUIRE(timedOut.HasError());
            CHECK(timedOut.ErrorValue().code.Value() == SaveErrors::OperationDeadlineExceeded.code.Value());
            REQUIRE(fixture.barrier->EndMutation(mutation, fixture.provenance.epoch).HasValue());
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Failed);
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
        }

        TEST_CASE("Legacy Save requests quiescence immediately and preserves its admission-time deadline",
                  "[save][session][preparation][legacy][deadline]") {
            PreparationFixture fixture;
            fixture.requiresPreparation = false;
            const auto request = Take(
                fixture.session->Save({.operation = 1, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 1}, fixture.writer));
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Pending);
            fixture.clock.now = Duration::FromMilliseconds(100);
            const auto timedOut = fixture.Pump();
            REQUIRE(timedOut.HasError());
            CHECK(timedOut.ErrorValue().code.Value() == SaveErrors::OperationDeadlineExceeded.code.Value());
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Failed);
            CHECK(fixture.captures == 0);
        }

        TEST_CASE("Deferred barrier admission failure terminalizes only its original producer and leaves foreign barrier ownership intact",
                  "[save][session][preparation][barrier-owner]") {
            PreparationFixture fixture;
            REQUIRE(fixture.barrier->Request(99, fixture.generation).HasValue());
            const auto request = fixture.Save(fixture.Preparation());
            CHECK_FALSE(request.Operation().Snapshot()->IsTerminal());
            const auto failed = fixture.Pump();
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == SaveErrors::OperationInProgress.code.Value());
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Failed);
            const auto foreign = Take(fixture.barrier->Snapshot());
            CHECK(foreign.operation == 99);
            CHECK(foreign.state == SaveBarrierState::Pending);
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
            REQUIRE(fixture.barrier->Cancel(99).HasValue());
            REQUIRE(fixture.barrier->Acknowledge(99).HasValue());
        }

        TEST_CASE("Cancellation remains effective while actual owner evidence is unready",
                  "[save][session][preparation][deferred][cancel]") {
            PreparationFixture fixture;
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &) {
                auto evidence = fixture.Evidence();
                evidence.ready = false;
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(evidence);
            });
            const auto request = fixture.Save(preparation);
            REQUIRE(fixture.Pump().HasValue());
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Idle);
            CHECK_FALSE(request.Operation().Snapshot()->IsTerminal());
            static_cast<void>(request.Operation().RequestCancellation());
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Cancelled);
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
            CHECK(Take(fixture.barrier->Snapshot()).state == SaveBarrierState::Idle);
        }

        TEST_CASE("Unready evidence cannot conceal stale provenance and cancellation still wins first",
                  "[save][session][preparation][deferred][stale]") {
            PreparationFixture fixture;
            bool cancel{};
            SECTION("stale unready evidence fails") {}
            SECTION("callback cancellation precedes stale evidence") {
                cancel = true;
            }
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &context) {
                if (cancel)
                    static_cast<void>(context.operation.RequestCancellation());
                auto evidence = fixture.Evidence();
                evidence.ready = false;
                ++evidence.provenance.sceneRevision;
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(evidence);
            });
            const auto request = fixture.Save(preparation);
            const auto pumped = fixture.Pump();
            if (cancel) {
                REQUIRE(pumped.HasValue());
            } else {
                REQUIRE(pumped.HasError());
                CHECK(pumped.ErrorValue().code.Value() == SaveErrors::GenerationStale.code.Value());
            }
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == (cancel ? SaveOperationState::Cancelled : SaveOperationState::Failed));
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
        }

        TEST_CASE("Owner preparation cancellation prevents any capture or worker commit", "[save][session][preparation]") {
            PreparationFixture fixture;
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &context) {
                (void)context.operation.RequestCancellation();
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(fixture.Evidence());
            });
            const auto request = fixture.Save(preparation);
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Cancelled);
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
        }

        TEST_CASE("Owner preparation cannot reenter admission and stale post-callback evidence fails closed",
                  "[save][session][preparation]") {
            PreparationFixture fixture;
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &) {
                const auto nested = fixture.session->Save({.operation = 2, .kind = SaveOperationKind::Save}, fixture.writer);
                CHECK(nested.HasError());
                auto evidence = fixture.Evidence();
                ++evidence.provenance.sceneRevision;
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(evidence);
            });
            const auto request = fixture.Save(preparation);
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Failed);
            CHECK(fixture.captures == 0);
        }

        TEST_CASE("Owner preparation failure and exceptions retain terminal original request ownership", "[save][session][preparation]") {
            PreparationFixture fixture;
            auto preparation = std::make_shared<OwnerPreparation>(
                [](const RuntimeSaveCapturePreparationContext &) -> Result<RuntimeSaveCapturePreparationEvidence> {
                return Result<RuntimeSaveCapturePreparationEvidence>::Failure(Error{ErrorCode{"test.prepare.failed"}});
            });
            SECTION("typed callback failure") {}
            SECTION("callback exception") {
                preparation = std::make_shared<OwnerPreparation>(
                    [](const RuntimeSaveCapturePreparationContext &) -> Result<RuntimeSaveCapturePreparationEvidence> {
                    throw std::runtime_error{"prepare failed"};
                });
            }
            const auto request = fixture.Save(preparation);
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Failed);
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.session->Acknowledge().HasValue());
        }

        TEST_CASE("Reentrant retirement is deferred through owner preparation failure or return", "[save][session][preparation]") {
            PreparationFixture fixture;
            bool fail = false;
            bool throwAfterRetire = false;
            SECTION("normal return") {}
            SECTION("typed failure after Retire") {
                fail = true;
            }
            SECTION("exception after Retire") {
                throwAfterRetire = true;
            }
            auto preparation = std::make_shared<OwnerPreparation>([&](const RuntimeSaveCapturePreparationContext &) {
                REQUIRE(fixture.session->Retire().HasValue());
                if (throwAfterRetire)
                    throw std::runtime_error{"retired preparation failed"};
                if (fail)
                    return Result<RuntimeSaveCapturePreparationEvidence>::Failure(Error{ErrorCode{"test.prepare.failed"}});
                return Result<RuntimeSaveCapturePreparationEvidence>::Success(fixture.Evidence());
            });
            const auto request = fixture.Save(preparation);
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Cancelled);
            CHECK(fixture.captures == 0);
            CHECK(fixture.writer->commits == 0);
            CHECK(fixture.session->IsSettled());
        }

        TEST_CASE("Legacy two-argument Save selects no preparation and preserves capture behavior", "[save][session][preparation]") {
            PreparationFixture fixture;
            fixture.requiresPreparation = false;
            const auto request = Take(
                fixture.session->Save({.operation = 1, .kind = SaveOperationKind::Save, .maximumCompletionCallbacks = 1}, fixture.writer));
            fixture.Finish(request);
            CHECK(fixture.captures == 1);
            CHECK(fixture.writer->commits == 1);
        }
    }  // namespace
}  // namespace Horo::Runtime
