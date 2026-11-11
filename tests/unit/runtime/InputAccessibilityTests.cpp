#include "Horo/Runtime/InputAccessibility.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Input {
    namespace {
        /** @brief Builds a complete wire document while letting malformed policy values reach the real parser unchanged. */
        std::string AccessibilityProfileJson(const std::string_view policy,
                                             const std::uint32_t version = CurrentInputBindingProfileSchema) {
            return std::string{"{\"schemaVersion\":"} + std::to_string(version) +
                   R"(,"profileId":"accessibility","overrides":[{"action":"game.action","bindings":[{"kind":0,"key":1,"accessibility":)" +
                   std::string{policy} + "}]}]}";
        }
    }  // namespace

    TEST_CASE("Binding accessibility persists through the canonical profile codec and legacy input upgrades once",
              "[input][accessibility][profile]") {
        const auto legacy = ParseBindingProfile(
            R"({"schemaVersion":1,"profileId":"legacy","overrides":[{"action":"game.action","bindings":[{"kind":0,"key":1}]}]})");
        REQUIRE(legacy.HasValue());
        REQUIRE(legacy.Value().schemaVersion == CurrentInputBindingProfileSchema);
        REQUIRE_FALSE(legacy.Value().overrides[0].bindings[0].accessibility.IsEnabled());
        auto profile = legacy.Value();
        auto &policy = profile.overrides[0].bindings[0].accessibility;
        policy.stickyModifiers = true;
        policy.digital = {.toggle = true,
                          .holdMicroseconds = 250000,
                          .repeat = true,
                          .repeatDelayMicroseconds = 500000,
                          .repeatIntervalMicroseconds = 75000};
        const auto encoded = SerializeBindingProfile(profile);
        REQUIRE(encoded.HasValue());
        const auto decoded = ParseBindingProfile(encoded.Value());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().overrides[0].bindings[0].accessibility == policy);
        REQUIRE(decoded.Value().schemaVersion == CurrentInputBindingProfileSchema);
        auto higher = profile;
        higher.overrides[0].bindings[0].accessibility.digital.holdMicroseconds = 100000;
        const auto merged = MergeBindingProfiles(profile, higher);
        REQUIRE(merged.HasValue());
        REQUIRE(merged.Value().overrides[0].bindings[0].accessibility.digital.holdMicroseconds == 100000);
        profile.schemaVersion = 1;
        REQUIRE(SerializeBindingProfile(profile).HasError());
        REQUIRE(ParseBindingProfile(AccessibilityProfileJson("{}", 1)).HasError());
        REQUIRE(ParseBindingProfile(AccessibilityProfileJson("{}", 3)).HasError());
    }

    TEST_CASE("Accessibility profile fields reject floating negative oversized unknown and coerced values",
              "[input][accessibility][profile]") {
        for (const std::string_view value : {"-1", "1.5", "true", "18446744073709551615", "1e100", "\"30\""}) {
            const auto document = AccessibilityProfileJson(std::string{"{\"holdMicroseconds\":"} + std::string{value} + "}");
            REQUIRE(ParseBindingProfile(document).HasError());
        }
        for (const std::string_view policy : {R"({"toggle":1})", R"({"stickyModifiers":"true"})", R"({"repeatIntervalMicroseconds":0})",
                                              R"({"unknown":true})", "[]", "null"})
            REQUIRE(ParseBindingProfile(AccessibilityProfileJson(policy)).HasError());
        REQUIRE(ParseBindingProfile(AccessibilityProfileJson(R"({"holdMicroseconds":60000000})")).HasValue());
        REQUIRE(ParseBindingProfile(AccessibilityProfileJson(R"({"holdMicroseconds":60000001})")).HasError());
        auto invalidSchema = AccessibilityProfileJson("{}");
        const auto number = invalidSchema.find(":2");
        REQUIRE(number != std::string::npos);
        invalidSchema.replace(number, 2, ":2.0");
        REQUIRE(ParseBindingProfile(invalidSchema).HasError());
    }

    TEST_CASE("Sticky physical overlap and modified digital policies cannot bypass the standard binding conflict validator",
              "[input][accessibility][validation]") {
        InputBinding sticky{.kind = BindingControlKind::Key, .key = Key::K, .requiredModifiers = {.control = true}};
        sticky.accessibility.stickyModifiers = true;
        const InputBinding plain{.kind = BindingControlKind::Key, .key = Key::K};
        const InputContextId context{"gameplay"};
        std::array actions{ActionDescriptor{ActionId{"game.sticky"}, ActionValueType::Digital, context, true, {sticky}},
                           ActionDescriptor{ActionId{"game.plain"}, ActionValueType::Digital, context, true, {plain}}};
        REQUIRE_FALSE(ValidateBindingProfile(actions, {}).IsValid());
        actions[0].defaultBindings[0].accessibility.stickyModifiers = false;
        REQUIRE(ValidateBindingProfile(actions, {}).IsValid());
        actions[0].defaultBindings[0].accessibility.digital.toggle = true;
        actions[0].valueType = ActionValueType::Axis1D;
        REQUIRE_FALSE(ValidateBindingProfile(actions, {}).IsValid());
        actions[0].valueType = ActionValueType::Digital;
        actions[0].defaultBindings[0].accessibility.digital.repeatIntervalMicroseconds = 0;
        REQUIRE_FALSE(ValidateBindingProfile(actions, {}).IsValid());
        actions[0].defaultBindings[0] = plain;
        actions[0].defaultBindings[0].accessibility.digital.toggle = true;
        REQUIRE_FALSE(ValidateBindingProfile(actions, {}).IsValid());
    }

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
