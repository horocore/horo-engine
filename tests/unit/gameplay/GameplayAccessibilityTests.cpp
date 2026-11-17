#include "Horo/Gameplay/GameplayAccessibility.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <new>

namespace Horo::Gameplay {
    namespace {
        ConfigurationSchema Schema() {
            ConfigurationSchema schema;
            const auto contribution = MakeGameplayAccessibilityContribution();
            REQUIRE(contribution.ownerPrefix == "accessibility.gameplay");
            for (const auto &setting : contribution.settings)
                REQUIRE(schema.Register(setting).HasValue());
            REQUIRE(schema.Seal().HasValue());
            return schema;
        }
    }  // namespace

    TEST_CASE("Gameplay tick captures authoritative assists without depending on bus delivery",
              "[gameplay][accessibility][configuration]") {
        const auto schema = Schema();
        ConfigurationResolutionRequest inputs;
        const auto first = ConfigurationResolver::Resolve(schema, inputs, 1).Value();
        const auto tick1 = GameplayAssistTickSnapshot::Capture(first);
        REQUIRE(tick1.HasValue());
        REQUIRE(tick1.Value().Preferences().damageMultiplier == 1);
        REQUIRE(tick1.Value().Preferences().reactionTimeMultiplier == 1);
        inputs.user.emplace(SettingKey{"accessibility.gameplay.aim_assist"}, ConfigurationInputValue{std::int64_t{750}, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.auto_aim_snap"}, ConfigurationInputValue{true, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.aggression_reduction"}, ConfigurationInputValue{std::int64_t{500}, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.reaction_time_multiplier"}, ConfigurationInputValue{std::int64_t{2000}, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.skip_qte"}, ConfigurationInputValue{true, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.post_hit_invulnerability_ms"},
                            ConfigurationInputValue{std::int64_t{250}, {}});
        inputs.user.emplace(SettingKey{"accessibility.gameplay.damage_multiplier"}, ConfigurationInputValue{std::int64_t{500}, {}});
        const auto next = ConfigurationResolver::Resolve(schema, inputs, 2).Value();
        REQUIRE(tick1.Value().Revision() == 1);
        REQUIRE(tick1.Value().Preferences().aimAssist == 0);
        REQUIRE_FALSE(tick1.Value().Preferences().skipQuickTimeEvents);
        // No bus is composed: omitted, dropped or coalesced hints cannot block convergence.
        const auto tick2 = GameplayAssistTickSnapshot::Capture(next);
        REQUIRE(tick2.HasValue());
        REQUIRE(tick2.Value().Revision() == 2);
        REQUIRE(tick2.Value().Preferences().aimAssist == 0.75F);
        REQUIRE(tick2.Value().Preferences().autoAimSnap);
        REQUIRE(tick2.Value().Preferences().aggressionReduction == 0.5F);
        REQUIRE(tick2.Value().Preferences().reactionTimeMultiplier == 2);
        REQUIRE(tick2.Value().Preferences().skipQuickTimeEvents);
        REQUIRE(tick2.Value().Preferences().postHitInvulnerabilitySeconds == 0.25F);
        REQUIRE(tick2.Value().Preferences().damageMultiplier == 0.5F);
        inputs.user[SettingKey{"accessibility.gameplay.damage_multiplier"}].value = std::int64_t{-1};
        const auto invalid = ConfigurationResolver::Resolve(schema, inputs, 3).Value();
        REQUIRE(GameplayAssistTickSnapshot::Capture(invalid).HasError());
        REQUIRE(tick2.Value().Revision() == 2);
        REQUIRE(tick2.Value().Preferences().damageMultiplier == 0.5F);
    }

    TEST_CASE("Gameplay hint allocation failure permits retry without changing authoritative preferences",
              "[gameplay][accessibility][bus]") {
        const auto schema = Schema();
        const auto snapshot = ConfigurationResolver::Resolve(schema, {}, 1).Value();
        EngineDataBus bus;
        GameplayAssistSettingsAdapter adapter{bus};
        std::size_t count = 0;
        const auto subscription = bus.Subscribe<GameplayAccessibilityStateEvent>([&](const auto &) {
            ++count;
        });
        bool failed = false;
        {
            Horo::Tests::AllocationProbe::ScopedFailure allocation;
            try {
                adapter.NotifyCommitted(snapshot);
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        REQUIRE(failed);
        REQUIRE(GameplayAssistTickSnapshot::Capture(snapshot).HasValue());
        adapter.NotifyCommitted(snapshot);
        bus.DispatchQueued();
        REQUIRE(count == 1);
    }

    TEST_CASE("Gameplay hint adapter emits monotonic revisions with no assist payload or stale replay", "[gameplay][accessibility][bus]") {
        const auto schema = Schema();
        const auto older = ConfigurationResolver::Resolve(schema, {}, 1).Value();
        const auto newer = ConfigurationResolver::Resolve(schema, {}, 3).Value();
        EngineDataBus bus;
        GameplayAssistSettingsAdapter adapter{bus};
        ConfigurationRevision last = 0;
        std::size_t count = 0;
        const auto subscription = bus.Subscribe<GameplayAccessibilityStateEvent>([&](const auto &event) {
            last = event.revision;
            ++count;
        });
        adapter.NotifyCommitted(newer);
        adapter.NotifyCommitted(older);
        adapter.NotifyCommitted(newer);
        REQUIRE(count == 0);
        bus.DispatchQueued();
        REQUIRE(count == 1);
        REQUIRE(last == 3);
        REQUIRE(GameplayAssistTickSnapshot::Capture(older).Value().Revision() == 1);
        REQUIRE(sizeof(GameplayAccessibilityStateEvent) == sizeof(ConfigurationRevision));
    }
}  // namespace Horo::Gameplay
