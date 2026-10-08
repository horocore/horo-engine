#include "SaveEventTriggerTestUtils.h"

#include <catch2/generators/catch_generators.hpp>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace EventTriggerTestSupport;

        TEST_CASE("Event trigger registrations validate finite allowlists and cooked policy", "[unit][save][event]") {
            Fixture fixture;
            const auto invalid = GENERATE(0, 1, 2, 3, 4, 5, 6);
            auto registrations = fixture.registrations;
            if (invalid == 0)
                registrations[0].id = {};
            if (invalid == 1)
                registrations[0].id = registrations[1].id;
            if (invalid == 2)
                registrations[0].mode = SavePolicyMode::Manual;
            if (invalid == 3)
                registrations[0].target.slot = {};
            if (invalid == 4)
                registrations[0].kind = static_cast<SaveTriggerKind>(255);
            if (invalid == 5)
                registrations[0].projectSchema = 9;
            if (invalid == 6)
                registrations[2].minimumPayloadBytes = 33;
            CHECK(SaveEventTriggers::Create(registrations, Policy(), fixture.host, fixture.arbiter, *fixture.safePoints).HasError());
        }

        TEST_CASE("Event payloads enforce registered variants schema and exact transition side", "[unit][save][event]") {
            Fixture fixture;
            const auto trigger = GENERATE(1ULL, 2ULL, 3ULL, 4ULL, 5ULL);
            auto event = fixture.Event(trigger);
            CHECK(fixture.triggers->Submit(event).HasValue());
            ++event.correlation.sequence;
            event.payload = SaveMilestonePayload{};
            CHECK(fixture.triggers->Submit(event).HasError());
        }

        TEST_CASE("Project trigger payloads reject unknown schema bounds and hidden trailing data", "[unit][save][event]") {
            Fixture fixture;
            auto event = fixture.Event(3);
            auto &payload = std::get<SaveProjectTriggerPayload>(event.payload);
            const auto invalid = GENERATE(0, 1, 2, 3);
            if (invalid == 0)
                payload.schema = 8;
            if (invalid == 1)
                payload.size = 0;
            if (invalid == 2)
                payload.size = 33;
            if (invalid == 3)
                payload.bytes.back() = std::byte{42};
            CHECK(fixture.triggers->Submit(event).HasError());
            CHECK(fixture.arbiter.QueuedCount() == 0);
        }

        TEST_CASE("Repeated rapid events retain one original correlation before and after admission", "[unit][save][event]") {
            Fixture fixture;
            const auto first = fixture.triggers->Submit(fixture.Event()).Value();
            for (std::uint64_t sequence = 2; sequence <= 1000; ++sequence)
                CHECK(fixture.triggers->Submit(fixture.Event(1, sequence)).Value().event.correlation == first.event.correlation);
            auto result = fixture.Poll();
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            const auto handoff = *result.Value();
            const auto duplicate = fixture.triggers->Submit(fixture.Event(1, 1001)).Value();
            CHECK(duplicate.operation.Id() == handoff.receipt.operation.Id());
            CHECK(duplicate.event.correlation == first.event.correlation);
            CHECK(fixture.arbiter.QueuedCount() == 0);
            CHECK(fixture.triggers->Submit(fixture.Event(1, 1)).HasError());
            CHECK(fixture.triggers->Submit(fixture.Event(2)).HasError());
            auto altered = fixture.Event(1, 1001);
            ++altered.generation.scene;
            CHECK(fixture.triggers->Submit(altered).HasError());
        }

        TEST_CASE("Event autosave defers behind other producers without losing its pending receipt", "[unit][save][event]") {
            Fixture fixture;
            REQUIRE(
                fixture.arbiter
                    .Admit({.operation = {.operation = 44, .maximumCompletionCallbacks = 1}, .address = fixture.registrations[0].target})
                    .HasValue());
            REQUIRE(fixture.triggers->Submit(fixture.Event()).HasValue());
            CHECK_FALSE(fixture.Poll().Value().has_value());
            CHECK(fixture.triggers->Receipt({{1}, 1}).Value().pending);
            CHECK(fixture.arbiter.Cancel(44) == SaveCancellationRequestResult::Requested);
            CHECK(fixture.Poll().Value().has_value());
        }

        TEST_CASE("Product cooldown is shared by trigger IDs rather than bypassed by another publisher", "[unit][save][event]") {
            Fixture fixture;
            fixture.Admit(fixture.Event());
            fixture.CaptureNow();
            fixture.Complete();
            REQUIRE(fixture.triggers->Submit(fixture.Event(2)).HasValue());
            CHECK_FALSE(fixture.Poll().Value().has_value());
            fixture.host.monotonicMilliseconds = 10;
            CHECK(fixture.Poll().Value().has_value());
        }

        TEST_CASE("Triggers reject unauthorized loading stale and unknown publisher requests", "[unit][save][event]") {
            Fixture fixture;
            const auto invalid = GENERATE(0, 1, 2, 3, 4, 5);
            auto event = fixture.Event();
            if (invalid == 0)
                fixture.host.authorized = false;
            if (invalid == 1)
                fixture.host.activity = SaveAutosaveActivity::Loading;
            if (invalid == 2)
                ++event.generation.registry;
            if (invalid == 3)
                event.correlation.trigger.value = 99;
            if (invalid == 4)
                event.correlation.sequence = 0;
            if (invalid == 5)
                fixture.host.binding.active.reset();
            CHECK(fixture.triggers->Submit(event).HasError());
            CHECK(fixture.arbiter.QueuedCount() == 0);
        }

        TEST_CASE("Event admission fences owner affinity safe phase and shutdown", "[unit][save][event]") {
            Fixture fixture;
            bool rejected{};
            std::jthread worker([&] {
                rejected = fixture.triggers->Submit(fixture.Event()).HasError() && fixture.Poll().HasError() &&
                           fixture.triggers->Receipt({{1}, 1}).HasError() && fixture.triggers->BeginShutdown().HasError();
            });
            worker.join();
            CHECK(rejected);
            REQUIRE(fixture.triggers->Submit(fixture.Event(4)).HasValue());
            CHECK(fixture.triggers->CommitAtSafePoint(RuntimePhase::VariableUpdate, {}).HasError());
            REQUIRE(fixture.triggers->BeginShutdown().HasValue());
            CHECK_FALSE(fixture.triggers->Receipt({{4}, 1}).Value().pending);
            CHECK(fixture.triggers->Submit(fixture.Event()).HasError());
            CHECK(fixture.triggers->BeginShutdown().HasValue());
        }
    }  // namespace
}  // namespace Horo::Runtime
