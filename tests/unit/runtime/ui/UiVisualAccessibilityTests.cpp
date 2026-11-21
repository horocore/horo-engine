#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiRenderSnapshot.h"
#include "Horo/Runtime/Ui/UiStyle.h"
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
        CHECK(second.Value().ScaleFactor() == UiCanvasScaleFactor{1, 2});
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

    TEST_CASE("Visual state indicators use distinct shapes and preserve exact presentation scopes", "[runtime_ui][accessibility][visual]") {
        const UiDrawCommand surface{.rect = {{-200, -300}, {6400, 2048}},
                                    .transform = 2,
                                    .clip = 3,
                                    .mask = 4,
                                    .opacity = 0.1F,
                                    .payload = UiSolidDraw{{1, 0, 0, 1}},
                                    .group = 5};
        std::array<UiDrawCommand, UiVisualIndicatorLimits::Commands> indicators;
        const auto states = (UiVisualState::Checked | UiVisualState::Selected) | UiVisualState::Focused | UiVisualState::Pressed |
                            UiVisualState::Disabled | UiVisualState::Invalid | UiVisualState::Busy;
        const auto result = ProjectUiVisualIndicators(states, surface, indicators);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == indicators.size());
        const auto &outer = std::get<UiBorderDraw>(indicators[0].payload);
        const auto &inner = std::get<UiBorderDraw>(indicators[1].payload);
        CHECK(outer.color == UiLinearColor{0, 0, 0, 1});
        CHECK(inner.color == UiLinearColor{1, 1, 1, 1});
        CHECK(indicators[1].rect.origin.x == surface.rect.origin.x + outer.width);
        for (const auto &mark : indicators) {
            CHECK(mark.transform == surface.transform);
            CHECK(mark.clip == surface.clip);
            CHECK(mark.mask == surface.mask);
            CHECK(mark.group == surface.group);
            CHECK(mark.opacity == 1.0F);
            CHECK(mark.rect.origin.x >= surface.rect.origin.x);
            CHECK(mark.rect.origin.y >= surface.rect.origin.y);
            CHECK(mark.rect.origin.x + mark.rect.extent.width <= surface.rect.origin.x + surface.rect.extent.width);
            CHECK(mark.rect.origin.y + mark.rect.extent.height <= surface.rect.origin.y + surface.rect.extent.height);
        }
        std::array<UiDrawCommand, UiVisualIndicatorLimits::Commands> disabled, selected;
        REQUIRE(ProjectUiVisualIndicators(UiVisualStateMask{UiVisualState::Disabled}, surface, disabled).Value() == 2);
        REQUIRE(ProjectUiVisualIndicators(UiVisualStateMask{UiVisualState::Selected} | UiVisualState::HighContrast, surface, selected)
                    .Value() == 2);
        CHECK(disabled[0].rect == selected[0].rect);
        CHECK(disabled[1].rect.extent.width > selected[1].rect.extent.width);
        CHECK(std::get<UiSolidDraw>(disabled[1].payload).color == std::get<UiSolidDraw>(selected[1].payload).color);
    }

    TEST_CASE("Visual indicator preparation is bounded and rejects overflowing layout without touching scratch",
              "[runtime_ui][accessibility][visual]") {
        UiDrawCommand surface{.rect = {{0, 0}, {2048, 2048}}, .payload = UiSolidDraw{}};
        std::array<UiDrawCommand, UiVisualIndicatorLimits::Commands> output;
        const UiVisualStateMask state{UiVisualState::Focused};
        output.front().transform = 42;
        const auto noCapacity = ProjectUiVisualIndicators(state, surface, std::span{output}.first(1));
        REQUIRE(noCapacity.HasError());
        CHECK(noCapacity.ErrorValue().code.Value() == UiErrors::CapacityExceeded.code.Value());
        surface.rect.origin.x = std::numeric_limits<std::int32_t>::max();
        const auto overflow = ProjectUiVisualIndicators(state, surface, output);
        REQUIRE(overflow.HasError());
        CHECK(overflow.ErrorValue().code.Value() == UiErrors::HandleMalformed.code.Value());
        CHECK(output.front().transform == 42);
        surface.rect = {{0, 0}, {0, 2048}};
        CHECK(ProjectUiVisualIndicators(state, surface, {}).Value() == 0);
        surface.rect = {{0, 0}, {2048, 2048}};
        surface.opacity = 0.0F;
        CHECK(ProjectUiVisualIndicators(state, surface, {}).Value() == 0);
        surface.opacity = 1.0F;
        CHECK(ProjectUiVisualIndicators(UiVisualStateMask{UiVisualState::HighContrast}, surface, {}).Value() == 0);
    }
}  // namespace Horo::Runtime::Ui
