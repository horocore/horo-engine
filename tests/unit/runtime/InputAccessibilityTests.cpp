#include "Horo/Runtime/InputAccessibility.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Input {
    TEST_CASE("Input affordances process deterministic randomized transitions without allocation", "[input][accessibility][allocation]") {
        auto state =
            DigitalAffordance::Create(
                {.toggle = true, .holdMicroseconds = 10, .repeat = true, .repeatDelayMicroseconds = 20, .repeatIntervalMicroseconds = 10})
                .Value();
        StickyModifierLatch sticky;
        bool physical = false;
        bool good = true;
        std::uint32_t random = 0x12345678;
        Horo::Tests::AllocationProbe::Measurement measurement;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            for (std::uint64_t frame = 1; frame < 10000; ++frame) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                const bool next = (random & 1) != 0;
                const ButtonState sample{next, next && !physical, physical && !next};
                const auto result = state.Update(sample, frame * 10);
                good &= !result.button.pressed || result.button.down;
                good &= !result.button.released || !result.button.down;
                sticky.Observe({.control = (random & 2) != 0, .shift = (random & 4) != 0});
                if (frame % 101 == 0) {
                    const auto release = state.Cancel();
                    good &= release.button.released == result.button.down;
                    sticky.Reset();
                }
                if (sample.pressed)
                    sticky.Consume();
                physical = next;
            }
            measurement = probe.Snapshot();
        }
        REQUIRE(good);
        REQUIRE(measurement.requests == 0);
    }

    TEST_CASE("Input toggles survive physical release and cancel with one semantic release", "[input][accessibility]") {
        auto state = DigitalAffordance::Create({.toggle = true}).Value();
        const auto press = state.Update({true, true, false}, 100);
        REQUIRE(press.button.down);
        REQUIRE(press.button.pressed);
        const auto up = state.Update({false, false, true}, 101);
        REQUIRE(up.button.down);
        REQUIRE_FALSE(up.button.released);
        const auto second = state.Update({true, true, false}, 102);
        REQUIRE_FALSE(second.button.down);
        REQUIRE(second.button.released);
        REQUIRE(state.Update({true, true, false}, 103).button.down);
        REQUIRE(state.Cancel().button.released);
        REQUIRE_FALSE(state.Cancel().button.released);
        REQUIRE_FALSE(state.Update({true, false, false}, 104).button.down);
    }

    TEST_CASE("Input hold and repeat thresholds are deterministic and do not replay missed intervals", "[input][accessibility]") {
        auto state =
            DigitalAffordance::Create(
                {.holdMicroseconds = 400000, .repeat = true, .repeatDelayMicroseconds = 200000, .repeatIntervalMicroseconds = 100000})
                .Value();
        REQUIRE_FALSE(state.Update({true, true, false}, 1000).button.down);
        REQUIRE_FALSE(state.Update({true, false, false}, 400999).button.down);
        const auto held = state.Update({true, false, false}, 401000);
        REQUIRE(held.button.down);
        REQUIRE(held.button.pressed);
        REQUIRE_FALSE(held.repeated);
        REQUIRE_FALSE(state.Update({true, false, false}, 600999).repeated);
        REQUIRE(state.Update({true, false, false}, 601000).repeated);
        REQUIRE(state.Update({true, false, false}, 10000000).repeated);
        REQUIRE_FALSE(state.Update({true, false, false}, 10000001).repeated);
        REQUIRE(state.Update({false, false, true}, 10000002).button.released);
        REQUIRE_FALSE(state.Update({true, true, false}, 10000003).button.down);
        REQUIRE_FALSE(state.Update({false, false, true}, 10000004).button.pressed);
        REQUIRE_FALSE(state.Update({true, false, false}, 20000000).button.down);
    }

    TEST_CASE("Input clock reversal and maximum-clock boundaries fail closed without duration overflow", "[input][accessibility]") {
        auto state = DigitalAffordance::Create({}).Value();
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(state.Update({true, true, false}, maximum - 2).button.down);
        REQUIRE(state.Update({true, false, false}, 1).button.released);
        REQUIRE_FALSE(state.Update({true, false, false}, 2).button.down);
        auto invalid = DigitalAffordancePolicy{};
        invalid.repeatIntervalMicroseconds = 0;
        REQUIRE(DigitalAffordance::Create(invalid).HasError());
        invalid = {};
        invalid.holdMicroseconds = maximum;
        REQUIRE(DigitalAffordance::Create(invalid).HasError());
        auto tap = DigitalAffordance::Create({}).Value();
        const auto pulse = tap.Update({false, true, true}, 0);
        REQUIRE_FALSE(pulse.button.down);
        REQUIRE(pulse.button.pressed);
        REQUIRE(pulse.button.released);
    }

    TEST_CASE("Sticky modifiers sequence without rewriting physical facts and clear at ownership boundaries", "[input][accessibility]") {
        StickyModifierLatch latch;
        const ModifierState control{.control = true};
        latch.Observe(control);
        REQUIRE(latch.Effective() == ModifierState{});
        latch.Observe({});
        latch.Observe(control);
        latch.Observe({});
        latch.Observe({.shift = true});
        latch.Observe({});
        REQUIRE(latch.Effective().control);
        REQUIRE(latch.Effective().shift);
        REQUIRE(control.control);
        REQUIRE_FALSE(control.shift);
        latch.Consume();
        REQUIRE(latch.Effective() == ModifierState{});
        latch.Observe(control);
        latch.Consume();
        REQUIRE(latch.Effective().control);
        latch.Reset();
        latch.Observe(control);
        REQUIRE(latch.Effective() == ModifierState{});
        latch.Observe({});
        latch.Observe(control);
        REQUIRE(latch.Effective().control);
    }
}  // namespace Horo::Input
