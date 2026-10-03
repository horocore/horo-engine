#include "Horo/Runtime/RuntimeLifecycle.h"
#include "Horo/Runtime/Save/SaveCaptureBarrier.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <future>
#include <stdexcept>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace CaptureTestSupport;

        class BarrierClock final : public Clock {
        public:
            Duration now;

            [[nodiscard]] Duration MonotonicNow() const override {
                return now;
            }
        };

        struct BarrierFixture final {
            BarrierClock clock;
            CanonicalStateParticipantRegistry registry;
            std::unique_ptr<SaveCaptureBarrier> barrier;
            SaveParticipantRegistrySnapshot participants;
            SaveRuntimeGeneration generation;
            RuntimeSaveCaptureProvenance provenance;
            std::array<std::size_t, 4> owners{};
            std::array<std::byte, 2> bytes{std::byte{0x11}, std::byte{0x22}};
            int captures{};
            CaptureCallback callback;

            explicit BarrierFixture(const SaveCaptureBarrierPolicy policy = {}) {
                barrier = SaveCaptureBarrier::Create(17, clock, 8, policy).Value();
                const std::array names{"project.capture", "horo.jobs", "horo.scene.mutation", "horo.subsystem"};
                for (std::size_t index = 0; index < owners.size(); ++index) {
                    owners[index] = barrier->Register(Participant(names[index]), static_cast<SaveBarrierDomain>(index)).Value();
                    REQUIRE(barrier->PublishReadiness(owners[index], {.value = 41}).HasValue());
                }
                auto adapter =
                    std::make_shared<CallbackCaptureAdapter>([this](const CanonicalCaptureContext &context, ICanonicalCaptureSink &sink) {
                    ++captures;
                    if (callback)
                        return callback(context, sink);
                    const auto written = sink.WriteCopied(Test::Id<SaveRecordId>(1), bytes);
                    REQUIRE(written.HasValue());
                    return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
                }, std::make_shared<int>());
                Register(registry, Descriptor("project.capture", {Test::Id<SaveRecordId>(1)}), std::move(adapter));
                participants = registry.Snapshot().Value();
                provenance = Provenance(participants);
                generation = {.runtime = 3, .scene = provenance.sceneIncarnation, .registry = participants.Generation()};
            }

            void Request(const OperationId operation = 91) {
                REQUIRE(barrier->Request(operation, generation).HasValue());
            }

            SaveCaptureBarrierOutcome Capture() {
                auto outcome =
                    barrier->CaptureAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, provenance, participants);
                REQUIRE(outcome.HasValue());
                return std::move(outcome).Value();
            }
        };

        class BarrierRuntimeHost final : public RuntimeLifecycleParticipant {
        public:
            explicit BarrierRuntimeHost(BarrierFixture &fixture) : fixture_(fixture) {}

            std::optional<RuntimeSaveSnapshot> detached;
            std::uint64_t ticks{};

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const FixedStepContext &context) override {
                auto ticket = fixture_.barrier->BeginMutation(0);
                if (ticket.HasError())
                    return Result<void>::Failure(ticket.ErrorValue());
                fixture_.bytes[0] = std::byte{static_cast<unsigned char>(context.simulationTick)};
                ticks = context.simulationTick;
                return fixture_.barrier->EndMutation(ticket.Value(), {.value = context.simulationTick + 41});
            }

            Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
                if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
                    return Result<void>::Success();
                fixture_.provenance.epoch.value = context.completedSimulationTick + 41;
                for (std::size_t index = 1; index < fixture_.owners.size(); ++index) {
                    auto ready = fixture_.barrier->PublishReadiness(index, fixture_.provenance.epoch);
                    if (ready.HasError())
                        return ready;
                }
                if (fixture_.barrier->Snapshot().Value().state != SaveBarrierState::Pending)
                    return Result<void>::Success();
                auto outcome = fixture_.barrier->CaptureAtSafePoint(phase, fixture_.generation, fixture_.provenance, fixture_.participants);
                if (outcome.HasError())
                    return Result<void>::Failure(outcome.ErrorValue());
                detached = outcome.Value().capture;
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                static_cast<void>(fixture_.barrier->BeginShutdown());
            }

        private:
            BarrierFixture &fixture_;
        };

        TEST_CASE("Save barrier uses real fixed scheduler commit and resumes next tick before worker encoding", "[unit][save][barrier]") {
            BarrierFixture fixture;
            RuntimeLifecycle lifecycle;
            auto host = std::make_unique<BarrierRuntimeHost>(fixture);
            auto *view = host.get();
            REQUIRE(lifecycle.AddParticipant(std::move(host)).HasValue());
            const CancellationToken token;
            REQUIRE(lifecycle.Startup(token).HasValue());
            auto scheduler = FrameScheduler::Create(fixture.clock).Value();
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
            fixture.Request();
            fixture.clock.now = Duration::FromNanoseconds(16'666'667);
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
            REQUIRE(view->detached.has_value());
            CHECK(view->ticks == 1);
            CHECK(view->detached->Provenance().epoch.value == 42);
            CHECK(view->detached->Records()[0].Segment(0)[0] == std::byte{1});
            fixture.clock.now += Duration::FromNanoseconds(16'666'667);
            REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
            CHECK(view->ticks == 2);
            CHECK(fixture.bytes[0] == std::byte{2});
            CHECK(view->detached->Records()[0].Segment(0)[0] == std::byte{1});
            lifecycle.Shutdown();
        }

        TEST_CASE("Save barrier hands off owned bytes and immediately resumes mutation", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.Request();
            const auto outcome = fixture.Capture();
            REQUIRE(outcome.barrier.state == SaveBarrierState::Captured);
            REQUIRE(outcome.capture.has_value());
            REQUIRE(outcome.capture->Records().size() == 1);
            CHECK(outcome.capture->Provenance() == fixture.provenance);
            CHECK(fixture.captures == 1);
            const auto mutation = fixture.barrier->BeginMutation(fixture.owners[0]).Value();
            fixture.bytes[0] = std::byte{0x77};
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 42}).HasValue());
            CHECK(outcome.capture->Records()[0].Segment(0)[0] == std::byte{0x11});
            auto worker = std::async(std::launch::async, [snapshot = *outcome.capture] {
                return snapshot.Records()[0].Segment(0)[0];
            });
            CHECK(worker.get() == std::byte{0x11});
            REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
        }

        TEST_CASE("Save barrier never captures a mutation and pending saves permit simulation progress", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.Request();
            const auto mutation = fixture.barrier->BeginMutation(fixture.owners[1]).Value();
            const auto waiting = fixture.Capture();
            CHECK(waiting.barrier.state == SaveBarrierState::Pending);
            CHECK(waiting.barrier.reason == SaveBarrierReason::Mutating);
            CHECK(waiting.barrier.participant == fixture.owners[1]);
            CHECK_FALSE(waiting.capture.has_value());
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 41}).HasValue());
            CHECK(fixture.Capture().barrier.state == SaveBarrierState::Captured);
        }

        TEST_CASE("Save barrier rejects spent foreign and overlapping mutation tickets", "[unit][save][barrier]") {
            BarrierFixture fixture;
            const auto first = fixture.barrier->BeginMutation(0).Value();
            CHECK(fixture.barrier->BeginMutation(0).HasError());
            CHECK(fixture.barrier->PublishReadiness(0, {.value = 41}).HasError());
            auto foreign = first;
            ++foreign.authority;
            CHECK(fixture.barrier->EndMutation(foreign, {.value = 41}).HasError());
            CHECK(fixture.barrier->EndMutation(first, {}).HasError());
            REQUIRE(fixture.barrier->EndMutation(first, {.value = 41}).HasValue());
            const auto second = fixture.barrier->BeginMutation(0).Value();
            CHECK(fixture.barrier->EndMutation(first, {.value = 41}).HasError());
            CHECK(fixture.barrier->BeginMutation(0).HasError());
            REQUIRE(fixture.barrier->EndMutation(second, {.value = 42}).HasValue());
            CHECK(fixture.barrier->EndMutation(second, {.value = 42}).HasError());
            CHECK(fixture.barrier->BeginMutation(9).HasError());
            CHECK(fixture.barrier->EndMutation({.authority = 17, .participant = 9, .serial = 1}, {.value = 41}).HasError());
            CHECK(fixture.barrier->PublishReadiness(0, {}).HasError());
            CHECK(fixture.barrier->PublishReadiness(0, {.value = 41}, SaveBarrierDenial::VersionUnavailable).HasError());
            CHECK(fixture.barrier->PublishReadiness(0, {}, static_cast<SaveBarrierDenial>(99)).HasError());
        }

        TEST_CASE("Save barrier requires current epoch after every mutation", "[unit][save][barrier]") {
            BarrierFixture fixture;
            const auto mutation = fixture.barrier->BeginMutation(0).Value();
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 40}).HasValue());
            fixture.Request();
            const auto stale = fixture.Capture();
            CHECK(stale.barrier.state == SaveBarrierState::Pending);
            CHECK(stale.barrier.reason == SaveBarrierReason::EpochUnavailable);
            CHECK(fixture.captures == 0);
            fixture.provenance.epoch.value = 42;
            for (const auto owner : fixture.owners)
                REQUIRE(fixture.barrier->PublishReadiness(owner, {.value = 42}).HasValue());
            CHECK(fixture.Capture().capture->Provenance().epoch.value == 42);
        }

        TEST_CASE("Save barrier denial and timeout obey typed policy", "[unit][save][barrier]") {
            const auto policy = GENERATE(SaveBarrierFailurePolicy::Defer, SaveBarrierFailurePolicy::Fail);
            const auto expected = policy == SaveBarrierFailurePolicy::Defer ? SaveBarrierState::Deferred : SaveBarrierState::Failed;
            BarrierFixture fixture({.failure = policy});
            fixture.Request();
            SECTION("Explicit denial") {
                const auto busy = fixture.barrier->BeginMutation(0).Value();
                REQUIRE(fixture.barrier->PublishReadiness(2, {}, SaveBarrierDenial::MutationCannotQuiesce).HasValue());
                const auto denied = fixture.Capture();
                CHECK(denied.barrier.state == expected);
                CHECK(denied.barrier.reason == SaveBarrierReason::Denied);
                CHECK(denied.barrier.denial == SaveBarrierDenial::MutationCannotQuiesce);
                REQUIRE(fixture.barrier->EndMutation(busy, {.value = 41}).HasValue());
            }
            SECTION("Deadline includes waiting across safe points") {
                fixture.clock.now = Duration::FromMilliseconds(100);
                const auto timedOut = fixture.Capture();
                CHECK(timedOut.barrier.state == expected);
                CHECK(timedOut.barrier.reason == SaveBarrierReason::Timeout);
                CHECK(timedOut.barrier.elapsed == fixture.clock.now);
                REQUIRE(timedOut.error.has_value());
                CHECK(timedOut.error->code.Value() == SaveErrors::OperationDeadlineExceeded.code.Value());
            }
            CHECK(fixture.captures == 0);
            CHECK(fixture.barrier->BeginMutation(0).HasValue());
        }

        TEST_CASE("Save barrier measures synchronous budget and discards late capture", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.callback = [&](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                fixture.clock.now = Duration::FromMilliseconds(6);
                REQUIRE(sink.WriteCopied(Test::Id<SaveRecordId>(1), fixture.bytes).HasValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            };
            fixture.Request();
            const auto late = fixture.Capture();
            CHECK(late.barrier.state == SaveBarrierState::Deferred);
            CHECK(late.barrier.reason == SaveBarrierReason::CaptureBudget);
            CHECK(late.barrier.captureDuration == Duration::FromMilliseconds(6));
            CHECK_FALSE(late.capture.has_value());
            REQUIRE(late.error.has_value());
            CHECK(late.error->code.Value() == SaveErrors::CaptureBudgetExceeded.code.Value());
            CHECK(fixture.barrier->BeginMutation(0).HasValue());
        }

        TEST_CASE("Save barrier rejects reentrant producer and lifecycle mutation during capture", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.callback = [&](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                CHECK(fixture.barrier->BeginMutation(0).HasError());
                CHECK(fixture.barrier->PublishReadiness(0, {.value = 42}).HasError());
                CHECK(fixture.barrier->Cancel(91).HasError());
                CHECK(fixture.barrier->BeginShutdown().HasError());
                CHECK(fixture.barrier->Acknowledge(91).HasError());
                CHECK(fixture.barrier->Register(Participant("project.other"), SaveBarrierDomain::Jobs).HasError());
                REQUIRE(sink.WriteCopied(Test::Id<SaveRecordId>(1), fixture.bytes).HasValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            };
            fixture.Request();
            CHECK(fixture.Capture().barrier.state == SaveBarrierState::Captured);
            CHECK(fixture.barrier->BeginMutation(0).HasValue());
        }

        TEST_CASE("Save barrier resumes after capture adapter failure or exception", "[unit][save][barrier]") {
            BarrierFixture fixture;
            SECTION("Typed failure is preserved") {
                fixture.callback = [](const CanonicalCaptureContext &, ICanonicalCaptureSink &) {
                    return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                };
            }
            SECTION("Unexpected exception is contained") {
                fixture.callback = [](const CanonicalCaptureContext &, ICanonicalCaptureSink &) -> Result<CanonicalCaptureDisposition> {
                    throw std::runtime_error("unexpected adapter failure");
                };
            }
            fixture.Request();
            const auto failed = fixture.Capture();
            CHECK(failed.barrier.state == SaveBarrierState::Failed);
            CHECK(failed.barrier.reason == SaveBarrierReason::CaptureFailure);
            CHECK(failed.error.has_value());
            CHECK_FALSE(failed.capture.has_value());
            CHECK(fixture.barrier->BeginMutation(0).HasValue());
        }

        TEST_CASE("Save barrier validates phase generation and actual registry ownership", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.Request();
            CHECK(
                fixture.barrier->CaptureAtSafePoint(RuntimePhase::FixedUpdate, fixture.generation, fixture.provenance, fixture.participants)
                    .HasError());
            auto generation = fixture.generation;
            ++generation.scene;
            CHECK(
                fixture.barrier
                    ->CaptureAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, fixture.provenance, fixture.participants)
                    .HasError());
            auto invalid = fixture.provenance;
            invalid.epoch = {};
            CHECK(fixture.barrier
                      ->CaptureAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation, invalid, fixture.participants)
                      .HasError());
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.barrier->Cancel(91).HasValue());
            REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
            Register(fixture.registry, Descriptor("project.unregistered", {Test::Id<SaveRecordId>(2)}, false), std::make_shared<int>());
            fixture.participants = fixture.registry.Snapshot().Value();
            fixture.provenance = Provenance(fixture.participants);
            fixture.generation.registry = fixture.participants.Generation();
            fixture.Request(92);
            CHECK(fixture.barrier
                      ->CaptureAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation, fixture.provenance,
                                           fixture.participants)
                      .HasError());
            CHECK(fixture.captures == 0);
        }

        TEST_CASE("Save barrier cancellation shutdown and acknowledgement preserve exact request ownership", "[unit][save][barrier]") {
            BarrierFixture fixture;
            fixture.Request();
            const auto ticket = fixture.barrier->BeginMutation(1).Value();
            CHECK(fixture.barrier->Cancel(92).HasError());
            CHECK(fixture.barrier->Acknowledge(91).HasError());
            CHECK(fixture.barrier->Request(92, fixture.generation).HasError());
            REQUIRE(fixture.barrier->Cancel(91).HasValue());
            REQUIRE(fixture.barrier->EndMutation(ticket, {.value = 41}).HasValue());
            REQUIRE(fixture.barrier->Acknowledge(91).HasValue());
            fixture.Request(92);
            const auto inFlight = fixture.barrier->BeginMutation(1).Value();
            REQUIRE(fixture.barrier->BeginShutdown().HasValue());
            REQUIRE(fixture.barrier->BeginShutdown().HasValue());
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Cancelled);
            CHECK(fixture.barrier->BeginMutation(0).HasError());
            CHECK(fixture.barrier->Request(93, fixture.generation).HasError());
            REQUIRE(fixture.barrier->EndMutation(inFlight, {.value = 41}).HasValue());
        }

        TEST_CASE("Save barrier enforces owner affinity and bounded complete domain registration", "[unit][save][barrier]") {
            BarrierClock clock;
            CHECK(SaveCaptureBarrier::Create(0, clock, 4).HasError());
            CHECK(SaveCaptureBarrier::Create(1, clock, 0).HasError());
            CHECK(SaveCaptureBarrier::Create(1, clock, 257).HasError());
            CHECK(SaveCaptureBarrier::Create(1, clock, 4, {.captureBudget = {}}).HasError());
            auto barrier = SaveCaptureBarrier::Create(1, clock, 1).Value();
            REQUIRE(barrier->Register(Participant("horo.fixed"), SaveBarrierDomain::FixedSimulation).HasValue());
            CHECK(barrier->Register(Participant("horo.fixed"), SaveBarrierDomain::FixedSimulation).HasError());
            CHECK(barrier->Register(Participant("horo.jobs"), SaveBarrierDomain::Jobs).HasError());
            CHECK(barrier->Request(1, {.runtime = 1, .scene = 1, .registry = 1}).HasError());
            auto worker = std::async(std::launch::async, [&] {
                return barrier->BeginMutation(0).HasError() && barrier->Snapshot().HasError() && barrier->BeginShutdown().HasError() &&
                       barrier->PublishReadiness(0, {.value = 41}).HasError();
            });
            CHECK(worker.get());
        }
    }  // namespace
}  // namespace Horo::Runtime
