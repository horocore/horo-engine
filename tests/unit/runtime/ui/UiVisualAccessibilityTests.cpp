#include "Horo/Runtime/Ui/UiVisualAccessibility.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Runtime::Ui {
    TEST_CASE("UI visual preferences capture scale and contrast atomically and preserve readable typography",
              "[runtime_ui][accessibility][visual]") {
        ConfigurationSchema schema;
        for (const auto &descriptor : MakeUiVisualAccessibilityContribution().settings)
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationResolutionRequest inputs;
        const auto first = UiVisualAccessibilitySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 1).Value());
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().TextSize(18).Value() == 18);
        inputs.user.emplace(SettingKey{"accessibility.visual.ui.ui_scale"}, ConfigurationInputValue{std::int64_t{500}, {}});
        inputs.user.emplace(SettingKey{"accessibility.visual.ui.high_contrast"}, ConfigurationInputValue{true, {}});
        const auto second = UiVisualAccessibilitySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 2).Value());
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().UiScale() == 0.5F);
        REQUIRE(second.Value().HighContrast());
        REQUIRE(second.Value().TextSize(18).Value() == 16);
        REQUIRE(second.Value().TextSize(40).Value() == 20);
        REQUIRE(first.Value().Revision() == 1);
        REQUIRE_FALSE(first.Value().HighContrast());
        REQUIRE(first.Value().TextSize(std::numeric_limits<float>::quiet_NaN()).HasError());
        REQUIRE(first.Value().TextSize(std::numeric_limits<float>::max()).HasError());
        inputs.user[SettingKey{"accessibility.visual.ui.ui_scale"}].value = std::int64_t{3001};
        REQUIRE(UiVisualAccessibilitySnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 3).Value()).HasError());
    }
}  // namespace Horo::Runtime::Ui
