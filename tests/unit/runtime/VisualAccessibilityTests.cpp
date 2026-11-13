#include "Horo/Accessibility/VisualAccessibility.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace Horo::Accessibility {
    namespace {
        ConfigurationSchema ColorSchema() {
            ConfigurationSchema schema;
            const auto contribution = MakeColorAccessibilityContribution();
            REQUIRE(contribution.ownerPrefix == "accessibility.colorblind");
            for (const auto &setting : contribution.settings)
                REQUIRE(schema.Register(setting).HasValue());
            REQUIRE(schema
                        .Register({SettingKey{"accessibility.visual.ui.high_contrast"}, SettingValueType::Boolean, false,
                                   SettingScope::User, ReloadPolicy::NextFrame, SettingSensitivity::Public, std::nullopt})
                        .HasValue());
            REQUIRE(schema.Seal().HasValue());
            return schema;
        }
    }  // namespace

    TEST_CASE("Color queries retain one immutable authority revision and reject malformed custom settings",
              "[accessibility][configuration]") {
        const auto schema = ColorSchema();
        ConfigurationResolutionRequest request;
        const auto initial = ConfigurationResolver::Resolve(schema, request, 1);
        REQUIRE(initial.HasValue());
        const auto first = ColorAccessibilitySnapshot::Capture(initial.Value());
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().GetActiveColorblindMode() == ColorblindMode::None);
        request.user.emplace(SettingKey{"accessibility.colorblind.mode"}, ConfigurationInputValue{std::int64_t{2}, {}});
        request.user.emplace(SettingKey{"accessibility.colorblind.severity"}, ConfigurationInputValue{std::int64_t{500}, {}});
        request.user.emplace(SettingKey{"accessibility.visual.ui.high_contrast"}, ConfigurationInputValue{true, {}});
        const auto updated = ConfigurationResolver::Resolve(schema, request, 2);
        REQUIRE(updated.HasValue());
        const auto second = ColorAccessibilitySnapshot::Capture(updated.Value());
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().Revision() == 2);
        REQUIRE(second.Value().GetActiveColorblindMode() == ColorblindMode::Deuteranopia);
        REQUIRE(second.Value().GetColorblindSeverity() == 0.5F);
        REQUIRE(second.Value().IsHighContrastEnabled());
        REQUIRE(first.Value().Revision() == 1);
        REQUIRE(first.Value().GetActiveColorblindMode() == ColorblindMode::None);
        REQUIRE_FALSE(first.Value().IsHighContrastEnabled());
        request.user.emplace(SettingKey{"accessibility.colorblind.custom.m00"}, ConfigurationInputValue{std::int64_t{4000001}, {}});
        const auto malformed = ConfigurationResolver::Resolve(schema, request, 3);
        REQUIRE(malformed.HasValue());
        REQUIRE(ColorAccessibilitySnapshot::Capture(malformed.Value()).HasError());
        REQUIRE(second.Value().Revision() == 2);
    }

    TEST_CASE("Color accessibility transforms preserve identity endpoints and admit bounded custom transforms", "[accessibility][color]") {
        ColorblindSettings settings;
        settings.mode = ColorblindMode::CustomMatrix;
        settings.custom.values = {0, 1, 0, 0, 0, 1, 1, 0, 0};
        REQUIRE(PrepareColorAccessibilityTransform(settings).Value().values == settings.custom.values);
        settings.severity = 0;
        REQUIRE(PrepareColorAccessibilityTransform(settings).Value().values == ColorMatrix{}.values);
        settings.severity = 0.5F;
        const auto half = PrepareColorAccessibilityTransform(settings).Value();
        REQUIRE(half.values[0] == 0.5F);
        REQUIRE(half.values[1] == 0.5F);
        REQUIRE(half.values[2] == 0);
        settings.mode = ColorblindMode::None;
        REQUIRE(PrepareColorAccessibilityTransform(settings).Value().values == ColorMatrix{}.values);
        settings.custom.values[8] = std::numeric_limits<float>::infinity();
        REQUIRE(PrepareColorAccessibilityTransform(settings).HasError());
        settings.custom = {};
        settings.severity = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(PrepareColorAccessibilityTransform(settings).HasError());
        settings.severity = 1.01F;
        REQUIRE(PrepareColorAccessibilityTransform(settings).HasError());
        settings.severity = 1;
        settings.custom.values[4] = -4.01F;
        REQUIRE(PrepareColorAccessibilityTransform(settings).HasError());
        settings.custom = {};
        settings.mode = static_cast<ColorblindMode>(255);
        REQUIRE(PrepareColorAccessibilityTransform(settings).HasError());
    }

    TEST_CASE("Dichromacy reference transforms match full-strength numerical samples", "[accessibility][color][reference]") {
        // Independent golden outputs for RGB=(0.2,0.4,0.8), from the published full-severity tables.
        const std::array modes{ColorblindMode::Protanopia, ColorblindMode::Deuteranopia, ColorblindMode::Tritanopia};
        const std::array<std::array<double, 3>, 3> expected{
            {{0.2875960, 0.4167858, 0.8215756}, {0.2353484, 0.3629478, 0.7899168}, {0.0773828, 0.474723, 0.5206134}}};
        constexpr std::array<double, 3> input{0.2, 0.4, 0.8};
        for (std::size_t mode = 0; mode < modes.size(); ++mode) {
            const auto matrix = PrepareColorAccessibilityTransform({modes[mode], 1, {}});
            REQUIRE(matrix.HasValue());
            for (std::size_t row = 0; row < 3; ++row) {
                double sample = 0;
                double neutral = 0;
                for (std::size_t col = 0; col < 3; ++col) {
                    sample += matrix.Value().values[3 * row + col] * input[col];
                    neutral += matrix.Value().values[3 * row + col];
                }
                REQUIRE(sample == Catch::Approx(expected[mode][row]).margin(0.000001));
                REQUIRE(neutral == Catch::Approx(1).margin(0.000002));
            }
        }
    }

}  // namespace Horo::Accessibility
