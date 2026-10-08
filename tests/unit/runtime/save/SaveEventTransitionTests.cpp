#include "SaveEventTriggerTestUtils.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Runtime {
    namespace {
        using namespace EventTriggerTestSupport;

        TEST_CASE("Transition saves capture the exact before source or after destination with async correlation", "[unit][save][event]") {
            Fixture fixture;
            const auto trigger = GENERATE(4ULL, 5ULL);
            const auto event = fixture.Event(trigger);
            const auto handoff = fixture.Admit(event);
            CHECK(DecideSaveTransition(handoff.receipt) == SaveTransitionDecision::Wait);
            fixture.CaptureNow();
            const auto &transition = std::get<SaveTransitionPayload>(handoff.receipt.event.payload);
            CHECK(fixture.captured == (trigger == 4 ? transition.source : transition.destination));
            CHECK(fixture.captures == 1);
            fixture.Complete();
            const auto receipt = fixture.triggers->Receipt(event.correlation).Value();
            CHECK(receipt.operation.Id() == handoff.receipt.operation.Id());
            CHECK(receipt.event.payload == event.payload);
            CHECK(DecideSaveTransition(receipt) == SaveTransitionDecision::Continue);
        }

        TEST_CASE("Transition failure and cancellation follow explicit continue or block policy", "[unit][save][event]") {
            Fixture fixture;
            const auto policy = GENERATE(SaveTransitionFailurePolicy::Continue, SaveTransitionFailurePolicy::Block);
            fixture.registrations[3].failure = policy;
            fixture.triggers =
                SaveEventTriggers::Create(fixture.registrations, Policy(), fixture.host, fixture.arbiter, *fixture.safePoints).Value();
            const auto handoff = fixture.Admit(fixture.Event(4));
            const auto cancelled = GENERATE(false, true);
            const auto operation = handoff.receipt.operation.Id();
            if (cancelled) {
                CHECK(fixture.arbiter.Cancel(operation) == SaveCancellationRequestResult::Requested);
                REQUIRE(fixture.arbiter.ObserveCancellation(operation).HasValue());
            } else {
                REQUIRE(fixture.arbiter.Fail(operation, MakeError(SaveErrors::StoragePermanentIo)).HasValue());
            }
            const auto receipt = fixture.triggers->Receipt({{4}, 1}).Value();
            CHECK(receipt.operation.Snapshot()->IsTerminal());
            CHECK(DecideSaveTransition(receipt) ==
                  (policy == SaveTransitionFailurePolicy::Continue ? SaveTransitionDecision::Continue : SaveTransitionDecision::Block));
            if (!cancelled)
                CHECK(receipt.operation.Snapshot()->terminalError->code.Value() == SaveErrors::StoragePermanentIo.code.Value());
        }

        TEST_CASE("Pending transition failures retain policy and prevent capture of the wrong incarnation", "[unit][save][event]") {
            Fixture fixture;
            fixture.registrations[3].failure = SaveTransitionFailurePolicy::Block;
            fixture.triggers =
                SaveEventTriggers::Create(fixture.registrations, Policy(), fixture.host, fixture.arbiter, *fixture.safePoints).Value();
            REQUIRE(fixture.triggers->Submit(fixture.Event(4)).HasValue());
            ++fixture.host.generation.scene;
            CHECK(fixture.Poll().HasError());
            const auto receipt = fixture.triggers->Receipt({{4}, 1}).Value();
            CHECK(receipt.error.has_value());
            CHECK(DecideSaveTransition(receipt) == SaveTransitionDecision::Block);
            CHECK(fixture.captures == 0);
        }

        TEST_CASE("Registered targets protect slot kinds capacity and namespace rebind", "[unit][save][event]") {
            Fixture fixture;
            const auto invalid = GENERATE(0, 1, 2, 3);
            REQUIRE(fixture.triggers->Submit(fixture.Event()).HasValue());
            if (invalid == 0)
                fixture.host.catalog.entries = {Test::Entry(1, 9)};
            if (invalid == 1) {
                auto entry = Test::Entry(2, 9);
                entry.publication.kind = SaveSlotKind::Auto;
                fixture.host.catalog.entries = {entry};
            }
            if (invalid == 2)
                ++fixture.host.binding.revision;
            if (invalid == 3)
                fixture.host.catalog.revision = 0;
            CHECK(fixture.Poll().HasError());
            CHECK(fixture.arbiter.QueuedCount() == 0);
            CHECK(fixture.triggers->Receipt({{1}, 1}).Value().error.has_value());
        }

        TEST_CASE("Capture and publication revalidation preserve namespace catalog generation and registered target",
                  "[unit][save][event]") {
            Fixture fixture;
            auto entry = Test::Entry(1, 9);
            entry.publication.kind = SaveSlotKind::Auto;
            fixture.host.catalog.entries = {entry};
            const auto handoff = fixture.Admit(fixture.Event());
            CHECK(handoff.expectedGeneration == Test::Id<SlotGenerationId>(9));
            REQUIRE(fixture.triggers->Revalidate(handoff).HasValue());
            const auto invalid = GENERATE(0, 1, 2, 3, 4);
            auto changed = handoff;
            if (invalid == 0)
                changed.target.slot = Test::Id<SaveGameSlotId>(22);
            if (invalid == 1)
                ++fixture.host.catalog.revision;
            if (invalid == 2)
                ++fixture.host.binding.revision;
            if (invalid == 3)
                fixture.host.catalog.entries[0].publication.generation = Test::Id<SlotGenerationId>(10);
            if (invalid == 4)
                ++fixture.host.generation.runtime;
            CHECK(fixture.triggers->Revalidate(changed).HasError());
        }

        TEST_CASE("Arbiter admission failure resolves transition policy and releases the pending intent", "[unit][save][event]") {
            Fixture fixture;
            const auto trigger = GENERATE(4ULL, 5ULL);
            const auto policy = GENERATE(SaveTransitionFailurePolicy::Continue, SaveTransitionFailurePolicy::Block);
            fixture.registrations[trigger - 1].failure = policy;
            fixture.arbiter = CreateSaveOperationArbiter({1}).Value();
            fixture.triggers =
                SaveEventTriggers::Create(fixture.registrations, Policy(), fixture.host, fixture.arbiter, *fixture.safePoints).Value();
            REQUIRE(
                fixture.arbiter
                    .Admit({.operation = {.operation = 44, .maximumCompletionCallbacks = 1}, .address = fixture.registrations[0].target})
                    .HasValue());
            REQUIRE(fixture.arbiter.Cancel(44) == SaveCancellationRequestResult::Requested);
            REQUIRE(fixture.arbiter.Snapshot(44)->operation.IsTerminal());
            REQUIRE_FALSE(fixture.arbiter.ActiveOperation().has_value());
            REQUIRE(fixture.arbiter.QueuedCount() == 0);
            const auto event = fixture.Event(trigger);
            REQUIRE(fixture.triggers->Submit(event).HasValue());

            const auto admitted = fixture.Poll();
            REQUIRE(admitted.HasError());
            CHECK(admitted.ErrorValue().code.Value() == SaveErrors::ArbiterCapacityExceeded.code.Value());
            const auto receipt = fixture.triggers->Receipt(event.correlation).Value();
            CHECK_FALSE(receipt.pending);
            REQUIRE(receipt.error.has_value());
            CHECK(receipt.error->code.Value() == admitted.ErrorValue().code.Value());
            CHECK_FALSE(receipt.operation.IsValid());
            CHECK(DecideSaveTransition(receipt) ==
                  (policy == SaveTransitionFailurePolicy::Continue ? SaveTransitionDecision::Continue : SaveTransitionDecision::Block));
            CHECK(fixture.captures == 0);
            CHECK(fixture.triggers->Submit(fixture.Event(2)).HasValue());
        }

        TEST_CASE("Safe-point fence admission failure terminalizes the queued save and remains observable", "[unit][save][event]") {
            Fixture fixture;
            REQUIRE(fixture.safePoints->BeginShutdown().HasValue());
            REQUIRE(fixture.triggers->Submit(fixture.Event(4)).HasValue());
            CHECK(fixture.Poll().HasError());
            const auto receipt = fixture.triggers->Receipt({{4}, 1}).Value();
            CHECK(receipt.error.has_value());
            CHECK(receipt.operation.Snapshot()->IsTerminal());
            CHECK(fixture.arbiter.QueuedCount() == 0);
            CHECK(DecideSaveTransition(receipt) == SaveTransitionDecision::Continue);
        }
    }  // namespace
}  // namespace Horo::Runtime
