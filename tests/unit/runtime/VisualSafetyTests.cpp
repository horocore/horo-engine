#include "Horo/Accessibility/VisualSafety.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Accessibility {
    TEST_CASE("Visual safety captures one revision and suppresses tagged effects independently of graphics quality",
              "[accessibility][safety]") {
        ConfigurationSchema schema;
        for (const auto &descriptor : MakeVisualSafetyContribution().settings)
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationResolutionRequest inputs;
        const auto initial = VisualSafetySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 1).Value());
        REQUIRE(initial.HasValue());
        REQUIRE(initial.Value().Scale(VisualEffectIntent::CameraShake) == 1);
        inputs.user.emplace(SettingKey{"accessibility.visual.safety.reduce_motion"}, ConfigurationInputValue{true, {}});
        inputs.user.emplace(SettingKey{"accessibility.visual.safety.flash_suppression"}, ConfigurationInputValue{true, {}});
        const auto next = VisualSafetySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 2).Value());
        REQUIRE(next.HasValue());
        REQUIRE(next.Value().Revision() == 2);
        REQUIRE(next.Value().ReduceMotion());
        REQUIRE(next.Value().SuppressFlash());
        REQUIRE(next.Value().Scale(VisualEffectIntent::CameraShake) == 0);
        REQUIRE(next.Value().Scale(VisualEffectIntent::Flash) == 0);
        REQUIRE(next.Value().Scale(VisualEffectIntent::DecorativeMotion) == 0);
        REQUIRE(next.Value().Scale(VisualEffectIntent::EssentialMotion) == 1);
        REQUIRE(next.Value().Scale(static_cast<VisualEffectIntent>(255)) == 0);
        REQUIRE(initial.Value().Revision() == 1);
        REQUIRE(initial.Value().Scale(VisualEffectIntent::Flash) == 1);
        inputs.user.emplace(SettingKey{"accessibility.visual.safety.camera_shake_multiplier"},
                            ConfigurationInputValue{std::int64_t{-1}, {}});
        REQUIRE(VisualSafetySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 3).Value()).HasError());
    }

}  // namespace Horo::Accessibility
