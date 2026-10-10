#include "Horo/Runtime/RuntimeLifecycle.h"
#include "SaveAutosaveTestUtils.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Runtime {
    namespace {
        using namespace AutosaveTestSupport;

        TEST_CASE("Autosave pending latest state survives active and queued manual precedence", "[unit][save][autosave]") {
            Fixture fixture;
            for (OperationId id : {11, 12}) {
                REQUIRE(fixture.arbiter
                            .Admit({.operation = fixture.Operation(id),
                                    .address = fixture.Address(),
                                    .priority = SaveArbiterPriority::UserBlocking,
                                    .conflict = SaveArbiterConflictPolicy::Queue})
                            .HasValue());
            }
            REQUIRE(fixture.arbiter.StartNext()->operation.operation == 11);
            REQUIRE(fixture.arbiter.Advance(11, SaveArbiterState::WaitingForSafePoint).HasValue());
            fixture.Sample(500);
            for (unsigned index = 0; index < 20; ++index) {
                const auto result = fixture.Poll();
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value());
            }
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.Snapshot().coalescedTriggers == 4);
            CHECK(fixture.arbiter.QueuedCount() == 1);
            REQUIRE(fixture.arbiter.Fail(11, MakeError(SaveErrors::CompositionInjectedFailure)).HasValue());
            REQUIRE(fixture.Poll().HasValue());
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.arbiter.StartNext()->operation.operation == 12);
            REQUIRE(fixture.arbiter.Fail(12, MakeError(SaveErrors::CompositionInjectedFailure)).HasValue());
            fixture.bytes[0] = std::byte{0x77};
            const auto captured = fixture.Capture();
            CHECK(captured.snapshot.Records()[0].Segment(0)[0] == std::byte{0x77});
            CHECK(fixture.arbiter.Snapshot(91)->mode == SavePolicyMode::Auto);
            CHECK(fixture.arbiter.Snapshot(91)->priority == SaveArbiterPriority::Background);
            CHECK_FALSE(fixture.Snapshot().pending);
            const auto mutation = fixture.barrier->BeginMutation(0).Value();
            fixture.bytes[0] = std::byte{0x88};
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 42}).HasValue());
            CHECK(captured.snapshot.Records()[0].Segment(0)[0] == std::byte{0x77});
        }

        TEST_CASE("Autosave cooldown bounds admission while exact timer grid continues", "[unit][save][autosave]") {
            Fixture fixture({.interval = Ns(100), .cooldown = Ns(250)});
            fixture.Sample(100);
            fixture.Capture();
            fixture.Sample(200);
            CHECK(fixture.Snapshot().pending);
            fixture.Complete(91);
            fixture.Sample(300);
            REQUIRE(fixture.Poll(92).HasValue());
            CHECK(fixture.captures == 1);
            CHECK(fixture.Snapshot().pendingAge == Ns(100));
            CHECK(fixture.Snapshot().cooldownRemaining == Ns(50));
            fixture.Sample(350);
            fixture.Capture(92);
            CHECK(fixture.Snapshot().untilNextTrigger == Ns(50));
            CHECK(fixture.Snapshot().coalescedTriggers == 1);
            CHECK(fixture.arbiter.QueuedCount() == 0);
        }

        TEST_CASE("Autosave barrier waits for committed mutation and captures current epoch once", "[unit][save][autosave]") {
            Fixture fixture;
            const auto mutation = fixture.barrier->BeginMutation(0).Value();
            fixture.Sample(100);
            auto pending = fixture.Poll();
            REQUIRE(pending.HasValue());
            CHECK_FALSE(pending.Value());
            CHECK(fixture.Snapshot().disposition == SaveAutosaveDisposition::Capturing);
            fixture.Sample(400);
            CHECK(fixture.Snapshot().pending);
            fixture.bytes[0] = std::byte{0x33};
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 42}).HasValue());
            fixture.provenance.epoch.value = 42;
            for (std::size_t index = 1; index < 4; ++index)
                REQUIRE(fixture.barrier->PublishReadiness(index, {.value = 42}).HasValue());
            const auto capture = fixture.Capture(999);
            CHECK(capture.operation.Id() == 91);
            CHECK(capture.snapshot.Provenance().epoch.value == 42);
            CHECK(capture.snapshot.Records()[0].Segment(0)[0] == std::byte{0x33});
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.captures == 1);
            CHECK(fixture.Poll(92).Value() == std::nullopt);
        }

        TEST_CASE("Autosave rejects stale phases and busy barrier without admitting an operation", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(100);
            CHECK(fixture.scheduler
                      ->CommitAtSafePoint(RuntimePhase::EndFrame, fixture.generation, {fixture.Operation(91), fixture.Address(), {}},
                                          fixture.provenance, fixture.participants)
                      .HasError());
            auto stale = fixture.generation;
            ++stale.scene;
            CHECK(fixture.scheduler
                      ->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, stale,
                                          {fixture.Operation(91), fixture.Address(), {}}, fixture.provenance, fixture.participants)
                      .HasError());
            REQUIRE(fixture.barrier->Request(55, fixture.generation).HasValue());
            REQUIRE(fixture.Poll().HasValue());
            CHECK_FALSE(fixture.arbiter.Snapshot(91));
            CHECK(fixture.Snapshot().pending);
            REQUIRE(fixture.barrier->Cancel(55).HasValue());
            REQUIRE(fixture.barrier->Acknowledge(55).HasValue());
            fixture.Capture();
        }

        TEST_CASE("Autosave bounded arbiter capacity failure retains pending intent and original cause", "[unit][save][autosave]") {
            Fixture fixture;
            for (OperationId id = 1; id <= 8; ++id) {
                REQUIRE(fixture.arbiter.Admit({.operation = fixture.Operation(id), .address = fixture.Address()}).HasValue());
                REQUIRE(fixture.arbiter.StartNext());
                REQUIRE(fixture.arbiter.Fail(id, MakeError(SaveErrors::CompositionInjectedFailure)).HasValue());
            }
            fixture.Sample(100);
            const auto full = fixture.Poll();
            REQUIRE(full.HasError());
            CHECK(full.ErrorValue().code.Value() == SaveErrors::ArbiterCapacityExceeded.code.Value());
            CHECK(fixture.Snapshot().pending);
            REQUIRE(fixture.arbiter.Acknowledge(1));
            fixture.Capture();
        }

        TEST_CASE("Autosave failed capture is visible and cannot cause automatic retry storms", "[unit][save][autosave]") {
            Fixture fixture;
            SECTION("Typed adapter cause") {
                fixture.callback = [](const CanonicalCaptureContext &, ICanonicalCaptureSink &) {
                    return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                };
            }
            SECTION("Arbitrary adapter exception") {
                fixture.callback = [](const CanonicalCaptureContext &, ICanonicalCaptureSink &) -> Result<CanonicalCaptureDisposition> {
                    throw AdapterFailure{};
                };
            }
            fixture.Sample(100);
            const auto failed = fixture.Poll();
            REQUIRE(failed.HasError());
            CHECK(fixture.arbiter.Snapshot(91)->operation.terminalError->code.Value() == failed.ErrorValue().code.Value());
            CHECK(fixture.Snapshot().blocked);
            CHECK(fixture.Snapshot().pending);
            fixture.Sample(1'000'000);
            REQUIRE(fixture.Poll(92).HasValue());
            CHECK(fixture.captures == 1);
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
            fixture.callback = {};
            REQUIRE(fixture.scheduler->Resume().HasValue());
            fixture.Capture(92);
        }

        TEST_CASE("Autosave deferred timeout retains bounded intent and exact barrier evidence", "[unit][save][autosave]") {
            Fixture fixture({}, {.quiesceTimeout = Ns(10)});
            REQUIRE(fixture.barrier->BeginMutation(0).HasValue());
            fixture.Sample(300'000'000'000);
            REQUIRE(fixture.Poll().HasValue());
            fixture.Sample(300'000'000'010);
            const auto failed = fixture.Poll();
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == SaveErrors::OperationDeadlineExceeded.code.Value());
            const auto state = fixture.Snapshot();
            CHECK(state.pending);
            CHECK(state.blocked);
            REQUIRE(state.lastBarrier);
            CHECK(state.lastBarrier->state == SaveBarrierState::Deferred);
            CHECK(state.lastBarrier->reason == SaveBarrierReason::Timeout);
            CHECK(state.lastBarrier->elapsed == Ns(10));
        }

        TEST_CASE("Autosave capture cancellation observes caller parent and deadline before adapters", "[unit][save][autosave]") {
            Fixture fixture;
            REQUIRE(fixture.barrier->BeginMutation(0).HasValue());
            CancellationSource source;
            auto descriptor = fixture.Operation(91);
            descriptor.parentCancellation = source.Token();
            fixture.Sample(100);
            REQUIRE(fixture.scheduler
                        ->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                            {descriptor, fixture.Address(), {}}, fixture.provenance, fixture.participants)
                        .HasValue());
            const auto reason = GENERATE(SaveCancellationReason::Caller, SaveCancellationReason::Parent);
            if (reason == SaveCancellationReason::Parent)
                source.RequestCancellation();
            else
                REQUIRE(fixture.arbiter.Snapshot(91)->operation.operation == 91);
            if (reason == SaveCancellationReason::Caller)
                CHECK(fixture.arbiter.Cancel(91) == SaveCancellationRequestResult::Requested);
            REQUIRE(fixture.Poll().HasValue());
            CHECK(fixture.captures == 0);
            CHECK(fixture.arbiter.Snapshot(91)->operation.state == SaveOperationState::Cancelled);
            CHECK(fixture.arbiter.Snapshot(91)->operation.cancellationReason == reason);
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
        }

        TEST_CASE("Autosave contains host clock exceptions and retains owned cleanup", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.clock.exception = GENERATE(HostClockException::Standard, HostClockException::Foreign, HostClockException::Integer);
            fixture.Sample(100);
            SECTION("Request boundary") {
                fixture.clock.throws = true;
                const auto failure = fixture.Poll();
                REQUIRE(failure.HasError());
                CHECK(failure.ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
                CHECK(fixture.Snapshot().blocked);
                CHECK(fixture.Snapshot().pending);
                CHECK_FALSE(fixture.arbiter.ActiveOperation());
                fixture.clock.throws = false;
                REQUIRE(fixture.scheduler->Resume().HasValue());
                fixture.Capture(92);
            }
            SECTION("Pending poll and cleanup boundary") {
                const auto mutation = fixture.barrier->BeginMutation(0).Value();
                REQUIRE(fixture.Poll().HasValue());
                fixture.clock.throws = true;
                CHECK(fixture.Poll().HasError());
                CHECK(fixture.Snapshot().blocked);
                const auto cancelled = fixture.scheduler->Cancel();
                REQUIRE(cancelled.HasError());
                CHECK(cancelled.ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
                CHECK_FALSE(fixture.arbiter.ActiveOperation());
                CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
                CHECK(fixture.Snapshot().lastBarrier->state == SaveBarrierState::Cancelled);
                fixture.clock.throws = false;
                REQUIRE(fixture.scheduler->Cancel().HasValue());
                CHECK_FALSE(fixture.arbiter.ActiveOperation());
                REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 41}).HasValue());
            }
        }

        TEST_CASE("Autosave teardown retires pending ownership when the host clock permanently fails", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.clock.exception = GENERATE(HostClockException::Standard, HostClockException::Foreign, HostClockException::Integer);
            const auto mutation = fixture.barrier->BeginMutation(0).Value();
            fixture.Sample(100);
            REQUIRE(fixture.Poll().HasValue());
            fixture.Sample(105);
            REQUIRE(fixture.Poll().HasValue());
            REQUIRE(fixture.barrier->Snapshot().Value().elapsed == Ns(5));
            REQUIRE(fixture.arbiter
                        .Admit({.operation = fixture.Operation(11),
                                .address = fixture.Address(),
                                .priority = SaveArbiterPriority::UserBlocking,
                                .conflict = SaveArbiterConflictPolicy::Queue})
                        .HasValue());
            fixture.clock.throws = true;
            SECTION("Explicit shutdown preserves a typed timing failure") {
                const auto closed = fixture.scheduler->BeginShutdown();
                REQUIRE(closed.HasError());
                CHECK(closed.ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
                CHECK(fixture.Snapshot().disposition == SaveAutosaveDisposition::Failed);
                REQUIRE(fixture.Snapshot().lastBarrier.has_value());
                CHECK(fixture.Snapshot().lastBarrier->state == SaveBarrierState::Cancelled);
                CHECK(fixture.Snapshot().lastBarrier->elapsed == Ns(5));
            }
            SECTION("Destruction without an explicit close") {
                fixture.scheduler.reset();
            }
            fixture.scheduler.reset();
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
            CHECK_FALSE(fixture.arbiter.ActiveOperation());
            CHECK(fixture.arbiter.Snapshot(91)->operation.state == SaveOperationState::Cancelled);
            CHECK(fixture.arbiter.QueuedCount() == 1);
            CHECK_FALSE(fixture.arbiter.Snapshot(11)->operation.cancellationRequested);
            CHECK(fixture.captures == 0);
            REQUIRE(fixture.barrier->EndMutation(mutation, {.value = 41}).HasValue());
            REQUIRE(fixture.barrier->BeginMutation(0).HasValue());
            REQUIRE(fixture.arbiter.StartNext()->operation.operation == 11);
            REQUIRE(fixture.arbiter.Advance(11, SaveArbiterState::WaitingForSafePoint).HasValue());
            CHECK_FALSE(fixture.arbiter.Snapshot(11)->operation.cancellationRequested);
        }

        TEST_CASE("Barrier cancellation timing failure releases only the exact pending request", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.clock.exception = GENERATE(HostClockException::Standard, HostClockException::Foreign, HostClockException::Integer);
            REQUIRE(fixture.barrier->Request(55, fixture.generation).HasValue());
            fixture.clock.throws = true;
            CHECK(fixture.barrier->Cancel(11).HasError());
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Pending);
            CHECK(fixture.barrier->Snapshot().Value().operation == 55);
            const auto cancelled = fixture.barrier->Cancel(55);
            const auto errorAllocations = Horo::Tests::AllocationProbe::Count() - fixture.clock.allocationsBeforeThrow;
            CHECK(errorAllocations == 0);
            REQUIRE(cancelled.HasError());
            CHECK(cancelled.ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Cancelled);
            CHECK(fixture.barrier->Snapshot().Value().elapsed == Duration{});
            REQUIRE(fixture.barrier->Acknowledge(55).HasValue());
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
            CHECK_FALSE(fixture.arbiter.ActiveOperation());
        }

        TEST_CASE("Autosave expired admission never invokes a capture adapter", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(100);
            auto descriptor = fixture.Operation(91);
            descriptor.deadline = std::chrono::steady_clock::time_point{};
            const auto result =
                fixture.scheduler->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation,
                                                     {descriptor, fixture.Address(), {}}, fixture.provenance, fixture.participants);
            REQUIRE(result.HasValue());
            CHECK_FALSE(result.Value());
            CHECK(fixture.captures == 0);
            CHECK(fixture.arbiter.Snapshot(91)->operation.cancellationReason == SaveCancellationReason::Deadline);
        }

        TEST_CASE("Autosave replacement fences old detached completion and resets intent", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(100);
            const auto old = fixture.Capture();
            fixture.Sample(200);
            REQUIRE(fixture.arbiter.Advance(91, SaveArbiterState::Committing, {1, 1}).HasValue());
            auto replacement = fixture.generation;
            ++replacement.scene;
            REQUIRE(fixture.scheduler->ReplaceSession({.generation = replacement}).HasValue());
            CHECK_FALSE(fixture.Snapshot().pending);
            CHECK(fixture.Snapshot().disposition == SaveAutosaveDisposition::Cancelled);
            REQUIRE(fixture.arbiter.Complete(91).HasValue());
            CHECK(old.operation.Snapshot()->state == SaveOperationState::Completed);
            CHECK(fixture.Poll(92).HasError());
            CHECK(fixture.Snapshot().triggers == 0);
            CHECK(fixture.Snapshot().generation == replacement);
            CHECK(fixture.scheduler->Sample({fixture.generation, Ns(300), Ns(300)}).HasError());
            REQUIRE(fixture.scheduler->Sample({replacement, Ns(100), Ns(100)}).HasValue());
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.Snapshot().operation == 0);
        }

        TEST_CASE("Autosave cancel shutdown and destruction preserve detached producer lifetime", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(100);
            SECTION("Uncaptured mutation") {
                REQUIRE(fixture.barrier->BeginMutation(0).HasValue());
                REQUIRE(fixture.Poll().HasValue());
                REQUIRE(fixture.scheduler->Cancel().HasValue());
                CHECK(fixture.arbiter.Snapshot(91)->operation.state == SaveOperationState::Cancelled);
            }
            SECTION("Detached worker") {
                const auto capture = fixture.Capture();
                fixture.scheduler.reset();
                CHECK(capture.operation.Snapshot()->cancellationRequested);
                CHECK(capture.snapshot.Records()[0].Segment(0)[0] == std::byte{1});
                CHECK(fixture.arbiter.Cancel(91) == SaveCancellationRequestResult::AlreadyRequested);
                REQUIRE(fixture.arbiter.ObserveCancellation(91).HasValue());
                CHECK(capture.operation.Snapshot()->state == SaveOperationState::Cancelled);
                return;
            }
            REQUIRE(fixture.scheduler->BeginShutdown().HasValue());
            REQUIRE(fixture.scheduler->BeginShutdown().HasValue());
            CHECK(fixture.Snapshot().disposition == SaveAutosaveDisposition::Closed);
            CHECK(fixture.scheduler->Resume().HasError());
            CHECK(fixture.scheduler->Sample({.generation = fixture.generation}).HasError());
        }

        TEST_CASE("Autosave failed detached storage blocks until explicit host recovery", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(100);
            fixture.Capture();
            REQUIRE(fixture.arbiter.Advance(91, SaveArbiterState::Committing, {1, 1}).HasValue());
            REQUIRE(fixture.arbiter.Fail(91, MakeError(SaveErrors::StoragePermanentIo), SaveOperationCommitOutcome::Unknown).HasValue());
            const auto observed = fixture.Poll(92);
            REQUIRE(observed.HasError());
            CHECK(observed.ErrorValue().code.Value() == SaveErrors::StoragePermanentIo.code.Value());
            CHECK(fixture.Snapshot().blocked);
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.arbiter.Snapshot(91)->operation.commit == SaveOperationCommitOutcome::Unknown);
            REQUIRE(fixture.Poll(92).HasValue());
            CHECK(fixture.captures == 1);
        }

        TEST_CASE("Autosave failed uncaptured work preserves external typed terminal evidence", "[unit][save][autosave]") {
            Fixture fixture;
            REQUIRE(fixture.barrier->BeginMutation(0).HasValue());
            fixture.Sample(100);
            REQUIRE(fixture.Poll().HasValue());
            REQUIRE(fixture.arbiter.Fail(91, MakeError(SaveErrors::GenerationStale)).HasValue());
            const auto observed = fixture.Poll(92);
            REQUIRE(observed.HasError());
            CHECK(observed.ErrorValue().code.Value() == SaveErrors::GenerationStale.code.Value());
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.Snapshot().blocked);
            CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Idle);
        }

        TEST_CASE("Autosave rejects adapter reentry and permits post-terminal owner observation", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.callback = [&fixture](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                CHECK(fixture.Poll(92).HasError());
                CHECK(fixture.scheduler->Cancel().HasError());
                CHECK(fixture.scheduler->BeginShutdown().HasError());
                CHECK(fixture.scheduler->Resume().HasError());
                CHECK(fixture.scheduler->Sample({fixture.generation, Ns(100), Ns(100)}).HasError());
                CHECK(fixture.scheduler->ReplaceSession({.generation = {.runtime = 4, .scene = 1, .registry = 1}}).HasError());
                REQUIRE(sink.WriteCopied(Test::Id<SaveRecordId>(1), fixture.bytes).HasValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            };
            fixture.Sample(100);
            const auto captured = fixture.Capture();
            CHECK(fixture.captures == 1);
            fixture.Sample(200);
            REQUIRE(captured.operation
                        .OnCompletion([&fixture](const SaveOperationSnapshot &) {
                CHECK(fixture.Poll(92).HasValue());
                CHECK(fixture.scheduler->Resume().HasValue());
            }).HasValue());
            REQUIRE(fixture.scheduler->Cancel().HasValue());
            REQUIRE(fixture.arbiter.ObserveCancellation(91).HasValue());
            CHECK(fixture.captures == 1);
        }

        class AutosaveRuntimeHost final : public RuntimeLifecycleParticipant {
        public:
            explicit AutosaveRuntimeHost(Fixture &fixture) : fixture_(fixture) {}

            const std::optional<SaveAutosaveCapture> &Detached() const noexcept {
                return detached_;
            }

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const FixedStepContext &context) override {
                const auto mutation = fixture_.barrier->BeginMutation(0);
                if (mutation.HasError())
                    return Result<void>::Failure(mutation.ErrorValue());
                fixture_.bytes[0] = std::byte{static_cast<unsigned char>(context.simulationTick)};
                return fixture_.barrier->EndMutation(mutation.Value(), {.value = context.simulationTick + 41});
            }

            Result<void> OnPhase(const RuntimePhase phase, const FrameContext &context) override {
                if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
                    return Result<void>::Success();
                if (const auto sampled = fixture_.scheduler->Sample(
                        {fixture_.generation, Ns(static_cast<std::int64_t>(context.completedSimulationTick) * 16'666'667),
                         fixture_.clock.now});
                    sampled.HasError())
                    return sampled;
                fixture_.provenance.epoch.value = context.completedSimulationTick + 41;
                for (std::size_t index = 1; index < 4; ++index) {
                    const auto ready = fixture_.barrier->PublishReadiness(index, fixture_.provenance.epoch);
                    if (ready.HasError())
                        return ready;
                }
                auto captured = fixture_.Poll();
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                if (captured.Value())
                    detached_ = *std::move(captured).Value();
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                static_cast<void>(fixture_.scheduler->BeginShutdown());
            }

        private:
            Fixture &fixture_;
            std::optional<SaveAutosaveCapture> detached_;
        };

        TEST_CASE("Autosave host captures through real lifecycle after fixed simulation and resumes detached", "[unit][save][autosave]") {
            Fixture fixture({.interval = Ns(16'666'667), .cooldown = {}});
            RuntimeLifecycle lifecycle;
            auto host = std::make_unique<AutosaveRuntimeHost>(fixture);
            const auto *view = host.get();
            REQUIRE(lifecycle.AddParticipant(std::move(host)).HasValue());
            CancellationToken cancellation;
            REQUIRE(lifecycle.Startup(cancellation).HasValue());
            const auto frames = FrameScheduler::Create(fixture.clock).Value();
            REQUIRE(frames->RunFrame(lifecycle, cancellation, false).HasValue());
            fixture.clock.now = Ns(16'666'667);
            REQUIRE(frames->RunFrame(lifecycle, cancellation, false).HasValue());
            REQUIRE(view->Detached());
            CHECK(view->Detached()->snapshot.Provenance().epoch.value == 42);
            CHECK(view->Detached()->snapshot.Records()[0].Segment(0)[0] == std::byte{1});
            fixture.clock.now = Ns(33'333'334);
            REQUIRE(frames->RunFrame(lifecycle, cancellation, false).HasValue());
            CHECK(fixture.bytes[0] == std::byte{2});
            CHECK(view->Detached()->snapshot.Records()[0].Segment(0)[0] == std::byte{1});
            CHECK(fixture.Snapshot().pending);
            lifecycle.Shutdown();
        }
    }  // namespace
}  // namespace Horo::Runtime
