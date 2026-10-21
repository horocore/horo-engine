#include "Horo/Extensions/EditorUiForm.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>

namespace Horo::Extensions {
    namespace {
        /** @brief Uses the same canonical numeric binding for semantic, gesture and budget regressions. */
        EditorUiForm ScalarForm() {
            EditorUiNumberNode number{.base = {.id = {"dial"},
                                               .label = {EditorUiTextKind::LocalizationKey, "dial.label"},
                                               .accessibleLabel = {EditorUiTextKind::LocalizationKey, "dial.accessible"},
                                               .custom = EditorUiCustomPresentation{.widget = EditorUiCustomWidget::ScalarDial,
                                                                                    .font = EditorThemeFontRole::SansEmphasis,
                                                                                    .icon = EditorThemeIconRole::Info}},
                                      .binding = {"gain"},
                                      .value = 25.0,
                                      .minimum = 0.0,
                                      .maximum = 100.0};
            return {.id = {"custom.form"}, .title = {EditorUiTextKind::LocalizationKey, "custom.title"}, .nodes = {{number}}};
        }
    }  // namespace
}  // namespace Horo::Extensions

namespace Horo::Extensions {
    TEST_CASE("Custom plot shares standard theme geometry and bounded semantic input", "[Extensions][Form][Plot]") {
        EditorUiPlotNode plot{.base = {.id = {"plot"}, .label = {EditorUiTextKind::LocalizationKey, "plot.label"}},
                              .action = {"inspect"},
                              .samples = {{0, 0.2F}, {0.5F, 0.8F}, {1, 0.3F}}};
        EditorUiForm form{.id = {"custom"}, .title = {EditorUiTextKind::LocalizationKey, "plot.title"}, .nodes = {{plot}}};
        const auto valid = ValidateEditorUiForm(form);
        INFO(valid.HasError() ? valid.ErrorValue().message : std::string{});
        REQUIRE(valid.HasValue());
        EditorThemeFrame theme;
        auto first = BuildEditorUiRenderSnapshot(form, theme, 80);
        REQUIRE(first.HasValue());
        CHECK(first.Value().nodes.front().kind == EditorUiNodeKind::Plot);
        CHECK(first.Value().nodes.front().focusable);
        theme.revision += 1;
        theme.uiScale = 2;
        theme.colors.accent = {1, 0, 0, 1};
        auto second = BuildEditorUiRenderSnapshot(form, theme, 80);
        REQUIRE(second.HasValue());
        CHECK(second.Value().nodes.front().height == first.Value().nodes.front().height * 2);
        CHECK(second.Value().themeRevision != first.Value().themeRevision);
        CHECK(second.Value().theme.colors.accent.red == 1);
        EditorUiPlotState state;
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Next, 0, true, theme, 0.1F));
        CHECK(state.selected == 1);
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Next, 0, false, theme, 0.1F));
        CHECK(state.selected == 1);
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Select, 0.95F, true, theme, 0.1F));
        CHECK(state.selected == 2);
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Activate, 0, false, theme, 0.1F));
        CHECK(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Activate, 0, true, theme, 0.1F));
        theme.accessibility.reduceMotion = true;
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::None, 0, false, theme, 0.1F));
        CHECK(state.emphasis == 0);
        plot.base.enabled = false;
        CHECK_FALSE(UpdateEditorUiPlot(plot, state, EditorUiPlotInput::Activate, 0, true, theme, 0.1F));
        plot.samples.resize(MaximumEditorUiPlotSamples + 1);
        CHECK(ValidateEditorUiNode({plot}).HasError());
    }
}  // namespace Horo::Extensions

namespace Horo::Extensions {
    TEST_CASE("Disabled ancestors remove custom input and focus from the common projection", "[Extensions][Form][Plot][Input]") {
        EditorUiContainerNode parent{.base = {.id = {"parent"}, .focusPolicy = EditorUiFocusPolicy::Never, .enabled = false}};
        EditorUiPlotNode plot{.base = {.id = {"plot"}, .parent = {"parent"}, .label = {EditorUiTextKind::LocalizationKey, "plot.label"}},
                              .action = {"inspect"},
                              .samples = {{0, 0}, {1, 1}}};
        EditorUiForm form{.id = {"custom"}, .title = {EditorUiTextKind::LocalizationKey, "plot.title"}, .nodes = {{parent}, {plot}}};
        const auto valid = ValidateEditorUiForm(form);
        INFO(valid.HasError() ? valid.ErrorValue().message : std::string{});
        REQUIRE(valid.HasValue());
        CHECK_FALSE(IsEditorUiNodeEnabled(form, {"plot"}));
        auto snapshot = BuildEditorUiRenderSnapshot(form, {}, 80);
        REQUIRE(snapshot.HasValue());
        CHECK_FALSE(snapshot.Value().nodes.back().enabled);
        CHECK_FALSE(snapshot.Value().nodes.back().focusable);
        CHECK(snapshot.Value().nodes.back().foregroundToken == EditorThemeColorRole::TextDisabled);
        std::get<EditorUiContainerNode>(form.nodes.front().payload).base.enabled = true;
        CHECK(IsEditorUiNodeEnabled(form, {"plot"}));
        CHECK_FALSE(IsEditorUiNodeEnabled(form, {"missing"}));
        std::get<EditorUiContainerNode>(form.nodes.front().payload).base.parent = {"plot"};
        CHECK_FALSE(IsEditorUiNodeEnabled(form, {"plot"}));
    }

    TEST_CASE("Custom scalar presentation preserves canonical values and exposes bounded semantic hooks",
              "[extensions][form][composition][accessibility]") {
        auto form = ScalarForm();
        const auto number = std::get<EditorUiNumberNode>(form.nodes.front().payload);
        REQUIRE(ValidateEditorUiForm(form).HasValue());
        CHECK(RequiresEditorUiComposition(form));
        EditorThemeFrame theme;
        auto rendered = BuildEditorUiRenderSnapshot(form, theme, 180);
        REQUIRE(rendered.HasValue());
        REQUIRE(rendered.Value().accessibility.size() == 1);
        const auto &semantic = rendered.Value().accessibility.front();
        CHECK(semantic.role == EditorUiAccessibleRole::ScalarDial);
        CHECK(semantic.name.value == "dial.accessible");
        CHECK(semantic.focusable);
        CHECK(std::get<double>(*semantic.value) == 25.0);
        EditorUiScalarState state;
        CHECK_FALSE(UpdateEditorUiScalar(number, state, EditorUiScalarInput::Begin, .75, true, theme, .01F));
        CHECK(state.editing);
        CHECK(state.preview == 75.0);
        CHECK(std::get<double>(number.value) == 25.0);
        CHECK_FALSE(UpdateEditorUiScalar(number, state, EditorUiScalarInput::Commit, .75, false, theme, .01F));
        CHECK_FALSE(state.editing);
        CHECK(state.preview == 25.0);
        CHECK_FALSE(UpdateEditorUiScalar(number, state, EditorUiScalarInput::Begin, .75, true, theme, .01F));
        const auto proposed = UpdateEditorUiScalar(number, state, EditorUiScalarInput::Commit, .75, true, theme, .01F);
        REQUIRE(proposed);
        CHECK(*proposed == 75.0);
        CHECK(std::get<double>(number.value) == 25.0);
        theme.accessibility.reduceMotion = true;
        CHECK_FALSE(UpdateEditorUiScalar(number, state, EditorUiScalarInput::Begin, .5, true, theme, .001F));
        CHECK(state.emphasis == 1.0F);
    }

    TEST_CASE("Custom composition budgets and unsupported presenter versions fail before publication",
              "[Extensions][Composition][Budget]") {
        auto form = ScalarForm();
        EditorUiFormLimits limits;
        limits.composition.maximumCustomWidgets = 0;
        CHECK(ValidateEditorUiForm(form, limits).HasError());
        limits = {};
        limits.composition.maximumPresentationStateBytes = 1;
        CHECK(ValidateEditorUiForm(form, limits).HasError());
        limits = {};
        limits.composition.maximumDrawSegments = 0;
        CHECK(ValidateEditorUiForm(form, limits).HasError());
        std::get<EditorUiNumberNode>(form.nodes.front().payload).base.custom->schemaVersion = 99;
        CHECK(ValidateEditorUiForm(form).HasError());
    }

    TEST_CASE("Scalar gestures retain finite endpoints and keyboard progress at decimal precision limits",
              "[Extensions][Composition][Scalar]") {
        auto form = ScalarForm();
        auto number = std::get<EditorUiNumberNode>(form.nodes.front().payload);
        number.minimum = 1.0;
        number.maximum = std::nextafter(1.0, 2.0);
        number.value = 1.0;
        EditorUiScalarState state;
        EditorThemeFrame theme;
        const auto stepped = UpdateEditorUiScalar(number, state, EditorUiScalarInput::Increment, 0, true, theme, .01F);
        REQUIRE(stepped);
        CHECK(*stepped == *number.maximum);
        number.maximum = std::numeric_limits<double>::max();
        number.minimum = *number.maximum / 2;
        number.value = *number.minimum;
        CHECK_FALSE(UpdateEditorUiScalar(number, state, EditorUiScalarInput::Begin, 1.0, true, theme, .01F));
        CHECK(std::isfinite(state.preview));
        CHECK(state.preview == *number.maximum);
    }

}  // namespace Horo::Extensions
