#include "SaveAutosaveTestUtils.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace AutosaveTestSupport;

        TEST_CASE("Autosave cadence preserves fractional partitions and jitter phase", "[unit][save][autosave]") {
            const std::int64_t step = GENERATE(1, 3, 7, 33, 100, 333, 1000);
            Fixture fixture({.interval = Ns(100), .cooldown = {}, .jitter = Ns(17), .jitterSeed = 17});
            for (std::int64_t now = step; now < 10'117; now += step)
                fixture.Sample(now);
            fixture.Sample(10'117);
            const auto state = fixture.Snapshot();
            CHECK(state.pending);
            CHECK(state.triggers == 101);
            CHECK(state.coalescedTriggers == 100);
            CHECK(state.untilNextTrigger == Ns(100));
            CHECK(state.pendingAge == Ns(10'000));
        }

        TEST_CASE("Autosave triggers at exact sub-millisecond boundaries without admission drift", "[unit][save][autosave]") {
            Fixture fixture({.interval = Ns(10), .cooldown = {}, .jitter = Ns(3), .jitterSeed = 7});
            fixture.Sample(12);
            CHECK_FALSE(fixture.Snapshot().pending);
            fixture.Sample(13);
            CHECK(fixture.Snapshot().pending);
            fixture.Sample(24);
            CHECK(fixture.Snapshot().triggers == 2);
            CHECK(fixture.Snapshot().untilNextTrigger == Ns(9));
            const auto capture = fixture.Capture();
            fixture.Complete(capture.operation.Id());
            fixture.Sample(32);
            REQUIRE(fixture.Poll(92).HasValue());
            CHECK_FALSE(fixture.Snapshot().pending);
            fixture.Sample(33);
            CHECK(fixture.Snapshot().triggers == 3);
            CHECK(fixture.Snapshot().pending);
        }

        TEST_CASE("Autosave pause loading inactive clock policy is explicit", "[unit][save][autosave]") {
            const auto activity = GENERATE(SaveAutosaveActivity::Paused, SaveAutosaveActivity::Loading, SaveAutosaveActivity::Inactive);
            const auto behavior = GENERATE(SaveAutosaveClockPolicy::Freeze, SaveAutosaveClockPolicy::Accumulate);
            Fixture fixture({.interval = Ns(100),
                             .cooldown = {},
                             .domain = SaveAutosaveTimeDomain::MonotonicRealTime,
                             .paused = behavior,
                             .loading = behavior,
                             .inactive = behavior});
            fixture.Sample(40, 40, activity);
            fixture.Sample(40, 1040, activity);
            CHECK(fixture.Snapshot().triggers == (behavior == SaveAutosaveClockPolicy::Freeze ? 0 : 10));
            REQUIRE(fixture.Poll().HasValue());
            CHECK(fixture.captures == 0);
            fixture.Sample(40, 1040, SaveAutosaveActivity::Active);
            fixture.Sample(100, 1100);
            CHECK(fixture.Snapshot().pending);
            CHECK(fixture.Capture().snapshot.Records()[0].Segment(0)[0] == std::byte{1});
        }

        TEST_CASE("Gameplay domain follows committed simulation time independently of real time", "[unit][save][autosave]") {
            using enum SaveAutosaveActivity;
            Fixture fixture({.interval = Ns(100), .cooldown = {}, .paused = SaveAutosaveClockPolicy::Accumulate});
            fixture.Sample(99, 1'000'000, Paused);
            fixture.Sample(99, 9'000'000, Paused);
            CHECK_FALSE(fixture.Snapshot().pending);
            fixture.Sample(99, 9'000'000, Active);
            fixture.Sample(100, 9'000'001);
            CHECK(fixture.Snapshot().triggers == 1);
        }

        TEST_CASE("Autosave suspension is constant space and has bounded missed trigger admission", "[unit][save][autosave]") {
            Fixture fixture({.interval = Ns(1), .cooldown = {}});
            constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
            fixture.Sample(maximum);
            auto state = fixture.Snapshot();
            CHECK(state.triggers == static_cast<std::uint64_t>(maximum));
            CHECK(state.pendingAge == Ns(maximum - 1));
            CHECK(state.untilNextTrigger == Ns(1));
            fixture.Capture();
            for (int index = 0; index < 10; ++index) {
                const auto result = fixture.Poll(92);
                REQUIRE(result.HasValue());
                CHECK_FALSE(result.Value().has_value());
            }
            CHECK(fixture.captures == 1);
            CHECK(fixture.arbiter.QueuedCount() == 0);
        }

        TEST_CASE("Autosave rejects backward invalid and stale samples atomically", "[unit][save][autosave]") {
            Fixture fixture;
            fixture.Sample(50);
            const auto invalid = GENERATE(0, 1, 2, 3);
            SaveAutosaveClockSample sample{fixture.generation, Ns(60), Ns(60)};
            if (invalid == 0)
                sample.gameplay = Ns(49);
            if (invalid == 1)
                sample.monotonic = Ns(-1);
            if (invalid == 2)
                sample.activity = static_cast<SaveAutosaveActivity>(255);
            if (invalid == 3)
                ++sample.generation.scene;
            CHECK(fixture.scheduler->Sample(sample).HasError());
            CHECK(fixture.Snapshot().untilNextTrigger == Ns(50));
            fixture.Sample(100);
            CHECK(fixture.Snapshot().triggers == 1);
        }

        TEST_CASE("Autosave policy validates finite exact arithmetic before construction", "[unit][save][autosave]") {
            Fixture fixture;
            SaveAutosavePolicy policy{.interval = Ns(100), .cooldown = {}};
            const auto invalid = GENERATE(0, 1, 2, 3, 4, 5, 6);
            if (invalid == 0)
                policy.interval = {};
            if (invalid == 1)
                policy.cooldown = Ns(-1);
            if (invalid == 2)
                policy.jitter = Ns(-1);
            if (invalid == 3)
                policy.interval = Ns(std::numeric_limits<std::int64_t>::max());
            if (invalid == 3)
                policy.jitter = Ns(1);
            if (invalid == 4)
                policy.domain = static_cast<SaveAutosaveTimeDomain>(255);
            if (invalid == 5)
                policy.loading = static_cast<SaveAutosaveClockPolicy>(255);
            SaveAutosaveClockSample initial{.generation = fixture.generation};
            if (invalid == 6)
                initial.gameplay = Ns(-1);
            CHECK(SaveAutosaveScheduler::Create(policy, initial, fixture.arbiter, *fixture.barrier).HasError());
        }

        TEST_CASE("Autosave maximal valid interval and jitter remain representable", "[unit][save][autosave]") {
            constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
            Fixture fixture({.interval = Ns(1),
                             .cooldown = Ns(maximum),
                             .jitter = Ns(maximum - 1),
                             .jitterSeed = static_cast<std::uint64_t>(maximum - 1)});
            CHECK(fixture.Snapshot().untilNextTrigger == Ns(maximum));
            fixture.Sample(maximum);
            CHECK(fixture.Snapshot().triggers == 1);
            fixture.Capture();
            CHECK(fixture.Snapshot().cooldownRemaining == Ns(maximum));
        }

        TEST_CASE("Autosave mutations and diagnostics reject nonowner threads", "[unit][save][autosave]") {
            Fixture fixture;
            bool rejected{};
            std::jthread worker([&fixture, &rejected] {
                rejected = fixture.scheduler->Sample({.generation = fixture.generation}).HasError() &&
                           fixture.scheduler->Snapshot().HasError() && fixture.scheduler->Cancel().HasError() &&
                           fixture.scheduler->Resume().HasError() && fixture.scheduler->BeginShutdown().HasError() &&
                           fixture.Poll().HasError();
            });
            worker.join();
            CHECK(rejected);
            CHECK_FALSE(fixture.Snapshot().pending);
        }
    }  // namespace
}  // namespace Horo::Runtime
