#include "Horo/Extensions/EditorUiForm.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Extensions::Tests {
    namespace {
        EditorUiText Localized(std::string value) {
            return EditorUiText{EditorUiTextKind::LocalizationKey, std::move(value)};
        }

        EditorUiText Technical(std::string value) {
            return EditorUiText{EditorUiTextKind::TechnicalText, std::move(value)};
        }

        EditorUiNodeBase Base(std::string id, std::string parent, std::string label,
                              const EditorUiFocusPolicy focus = EditorUiFocusPolicy::Automatic) {
            return EditorUiNodeBase{.id = EditorUiId{std::move(id)},
                                    .parent = EditorUiId{std::move(parent)},
                                    .label = label.empty() ? EditorUiText{} : Localized(std::move(label)),
                                    .description = {},
                                    .accessibleLabel = {},
                                    .size = EditorUiComponentSize::Medium,
                                    .tone = EditorUiSemanticTone::Neutral,
                                    .focusPolicy = focus,
                                    .enabled = true,
                                    .readOnly = false};
        }

        void AddReferenceBasics(EditorUiFormBuilder &builder) {
            REQUIRE(builder
                        .AddContainer(EditorUiContainerNode{.base = Base("layout", {}, "examples.reference_form.settings",
                                                                         EditorUiFocusPolicy::Never),
                                                            .layout = EditorUiLayoutKind::Group,
                                                            .columns = 1})
                        .HasValue());
            REQUIRE(builder
                        .AddLabel(EditorUiLabelNode{.base = Base("intro", "layout", {}, EditorUiFocusPolicy::Never),
                                                    .text = Localized("examples.reference_form.intro")})
                        .HasValue());
            REQUIRE(builder
                        .AddText(EditorUiTextNode{.base = Base("summary", "layout", {}, EditorUiFocusPolicy::Never),
                                                  .text = Technical("headless-compatible preview")})
                        .HasValue());
            REQUIRE(builder
                        .AddTextField(EditorUiTextFieldNode{.base = Base("name", "layout", "examples.reference_form.name"),
                                                            .binding = EditorUiBindingId{"settings.name"},
                                                            .value = "Sample",
                                                            .placeholder = Localized("examples.reference_form.name_placeholder"),
                                                            .maximumBytes = 64,
                                                            .multiline = false})
                        .HasValue());
            REQUIRE(builder
                        .AddNumber(EditorUiNumberNode{.base = Base("quality", "layout", "examples.reference_form.quality"),
                                                      .binding = EditorUiBindingId{"settings.quality"},
                                                      .numberKind = EditorUiNumberKind::Integer,
                                                      .value = std::int64_t{2},
                                                      .minimum = 0.0,
                                                      .maximum = 4.0,
                                                      .step = 1.0})
                        .HasValue());
            REQUIRE(builder
                        .AddBoolean(EditorUiBooleanNode{.base = Base("enabled", "layout", "examples.reference_form.enabled"),
                                                        .binding = EditorUiBindingId{"settings.enabled"},
                                                        .value = true})
                        .HasValue());
        }

        void AddReferencePresentation(EditorUiFormBuilder &builder) {
            REQUIRE(builder
                        .AddChoice(EditorUiChoiceNode{.base = Base("profile", "layout", "examples.reference_form.profile"),
                                                      .binding = EditorUiBindingId{"settings.profile"},
                                                      .selected = "balanced",
                                                      .options = {{"fast", Localized("examples.reference_form.profile.fast"), true},
                                                                  {"balanced", Localized("examples.reference_form.profile.balanced"), true},
                                                                  {"quality", Localized("examples.reference_form.profile.quality"), true}},
                                                      .allowEmpty = false})
                        .HasValue());
            REQUIRE(builder
                        .AddPath(EditorUiPathNode{.base = Base("output", "layout", "examples.reference_form.output"),
                                                  .binding = EditorUiBindingId{"settings.output"},
                                                  .value = "assets/generated",
                                                  .pathKind = EditorUiPathKind::Directory,
                                                  .allowRelative = true})
                        .HasValue());
            REQUIRE(builder
                        .AddColor(EditorUiColorNode{.base = Base("tint", "layout", "examples.reference_form.tint"),
                                                    .binding = EditorUiBindingId{"settings.tint"},
                                                    .value = EditorUiColorValue{0.2F, 0.4F, 0.8F, 1.0F},
                                                    .allowAlpha = false})
                        .HasValue());
            REQUIRE(builder
                        .AddVector(EditorUiVectorNode{.base = Base("offset", "layout", "examples.reference_form.offset"),
                                                      .binding = EditorUiBindingId{"settings.offset"},
                                                      .value = EditorUiVectorValue{{1.0, 2.0, 3.0, 0.0}, 3}})
                        .HasValue());
            REQUIRE(builder
                        .AddAction(EditorUiActionNode{.base = Base("apply", "layout", "examples.reference_form.apply"),
                                                      .action = EditorUiActionId{"actions.apply"},
                                                      .actionKind = EditorUiActionKind::Primary,
                                                      .requiresConfirmation = false})
                        .HasValue());
        }

        void AddReferenceFeedback(EditorUiFormBuilder &builder) {
            REQUIRE(builder
                        .AddValidation(EditorUiValidationNode{.base = Base("quality_warning", "quality", {}, EditorUiFocusPolicy::Never),
                                                              .severity = EditorUiValidationSeverity::Warning,
                                                              .code = "quality.out_of_date",
                                                              .message = Localized("examples.reference_form.quality_warning")})
                        .HasValue());
            REQUIRE(builder
                        .AddHelp(EditorUiHelpNode{.base = Base("help", "layout", {}, EditorUiFocusPolicy::Never),
                                                  .text = Localized("examples.reference_form.help")})
                        .HasValue());
        }

        EditorUiForm BuildReferenceForm() {
            auto builderResult =
                EditorUiFormBuilder::Create(EditorUiId{"com.example.reference.form"}, Localized("examples.reference_form.title"));
            REQUIRE(builderResult.HasValue());
            auto builder = std::move(builderResult).Value();

            AddReferenceBasics(builder);
            AddReferencePresentation(builder);
            AddReferenceFeedback(builder);

            auto form = std::move(builder).Build();
            REQUIRE(form.HasValue());
            return std::move(form).Value();
        }

        EditorUiForm BuildResponsiveForm() {
            auto builderResult =
                EditorUiFormBuilder::Create(EditorUiId{"com.example.responsive.form"}, Localized("examples.responsive.title"));
            REQUIRE(builderResult.HasValue());
            auto builder = std::move(builderResult).Value();
            REQUIRE(builder
                        .AddContainer(EditorUiContainerNode{.base = Base("row", {}, {}, EditorUiFocusPolicy::Never),
                                                            .layout = EditorUiLayoutKind::Row,
                                                            .columns = 1})
                        .HasValue());
            REQUIRE(builder
                        .AddTextField(EditorUiTextFieldNode{.base = Base("left", "row", "examples.responsive.left"),
                                                            .binding = EditorUiBindingId{"left"},
                                                            .value = "a",
                                                            .placeholder = {},
                                                            .maximumBytes = 16,
                                                            .multiline = false})
                        .HasValue());
            REQUIRE(builder
                        .AddTextField(EditorUiTextFieldNode{.base = Base("right", "row", "examples.responsive.right"),
                                                            .binding = EditorUiBindingId{"right"},
                                                            .value = "b",
                                                            .placeholder = {},
                                                            .maximumBytes = 16,
                                                            .multiline = false})
                        .HasValue());
            auto form = std::move(builder).Build();
            REQUIRE(form.HasValue());
            return std::move(form).Value();
        }

        void RequireError(const Result<void> &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Reference extension composes every standard primitive without GUI dependencies", "[Extensions][EditorUiForm]") {
        const EditorUiForm form = BuildReferenceForm();
        REQUIRE(ValidateEditorUiForm(form).HasValue());
        REQUIRE(form.nodes.size() == 13);

        const std::array expectedKinds{EditorUiNodeKind::Group,     EditorUiNodeKind::Label,  EditorUiNodeKind::Text,
                                       EditorUiNodeKind::TextField, EditorUiNodeKind::Number, EditorUiNodeKind::Boolean,
                                       EditorUiNodeKind::Choice,    EditorUiNodeKind::Path,   EditorUiNodeKind::Color,
                                       EditorUiNodeKind::Vector,    EditorUiNodeKind::Action, EditorUiNodeKind::Validation,
                                       EditorUiNodeKind::Help};
        for (std::size_t index = 0; index < expectedKinds.size(); ++index)
            CHECK(EditorUiNodeKindOf(form.nodes[index]) == expectedKinds[index]);
        CHECK(EditorUiNodeBaseOf(form.nodes[3]).id.value == "name");
        CHECK(EditorUiNodeBaseOf(form.nodes[3]).parent.value == "layout");
    }

    TEST_CASE("Form validation rejects unstable identities, mismatched values, and invalid trees", "[Extensions][EditorUiForm]") {
        auto form = BuildReferenceForm();

        form.nodes[3].payload = EditorUiTextFieldNode{.base = Base("name", "layout", "examples.reference_form.name"),
                                                      .binding = EditorUiBindingId{"settings.quality"},
                                                      .value = "Sample",
                                                      .placeholder = {},
                                                      .maximumBytes = 64,
                                                      .multiline = false};
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");

        form = BuildReferenceForm();
        std::get<EditorUiNumberNode>(form.nodes[4].payload).value = 2.5;
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");

        form = BuildReferenceForm();
        std::get<EditorUiChoiceNode>(form.nodes[6].payload).selected = "missing";
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");

        form = BuildReferenceForm();
        std::get<EditorUiVectorNode>(form.nodes[9].payload).value.dimension = 5;
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");

        form = BuildReferenceForm();
        std::get<EditorUiTextNode>(form.nodes[2].payload).base.parent = EditorUiId{"name"};
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");

        form = BuildReferenceForm();
        std::get<EditorUiActionNode>(form.nodes[10].payload).base.readOnly = true;
        RequireError(ValidateEditorUiForm(form), "editor_ui_form_invalid");
    }

    TEST_CASE("Builder enforces finite form capacity and preserves typed node ownership", "[Extensions][EditorUiForm]") {
        EditorUiFormLimits limits;
        limits.maximumNodes = 1;
        auto builderResult = EditorUiFormBuilder::Create(EditorUiId{"com.example.small"}, Localized("examples.small.title"), limits);
        REQUIRE(builderResult.HasValue());
        auto builder = std::move(builderResult).Value();
        REQUIRE(builder
                    .AddLabel(EditorUiLabelNode{.base = Base("label", {}, {}, EditorUiFocusPolicy::Never),
                                                .text = Localized("examples.small.label")})
                    .HasValue());
        const auto second = builder.AddHelp(
            EditorUiHelpNode{.base = Base("help", {}, {}, EditorUiFocusPolicy::Never), .text = Localized("examples.small.help")});
        RequireError(second, "editor_ui_form_capacity_exceeded");
        auto form = std::move(builder).Build();
        REQUIRE(form.HasValue());
        CHECK(form.Value().nodes.front().payload.index() == 0);
    }

    TEST_CASE("Theme changes and UI scale update every standard control through semantic tokens", "[Extensions][EditorUiForm]") {
        const EditorUiForm form = BuildReferenceForm();
        EditorUiThemeFrame firstTheme;
        firstTheme.revision = 7;
        firstTheme.uiScale = 1.0F;
        firstTheme.metrics = {.smallControlHeight = 18.0F,
                              .mediumControlHeight = 30.0F,
                              .largeControlHeight = 42.0F,
                              .textLineHeight = 16.0F,
                              .rowGap = 5.0F,
                              .defaultWidth = 360.0F};
        const auto first = BuildEditorUiRenderSnapshot(form, firstTheme, 320.0F);
        REQUIRE(first.HasValue());

        EditorUiThemeFrame secondTheme = firstTheme;
        secondTheme.revision = 8;
        secondTheme.uiScale = 2.0F;
        secondTheme.metrics.mediumControlHeight = 34.0F;
        const auto second = BuildEditorUiRenderSnapshot(form, secondTheme, 320.0F);
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().nodes.size() == first.Value().nodes.size());
        CHECK(second.Value().themeRevision == 8);
        CHECK(second.Value().uiScale == Catch::Approx(2.0F));
        CHECK(second.Value().availableWidth == Catch::Approx(640.0F));

        for (std::size_t index = 0; index < first.Value().nodes.size(); ++index) {
            CHECK(second.Value().nodes[index].id == first.Value().nodes[index].id);
            CHECK(second.Value().nodes[index].kind == first.Value().nodes[index].kind);
            CHECK(second.Value().nodes[index].height >= first.Value().nodes[index].height * 1.5F);
            CHECK(second.Value().nodes[index].foregroundToken != EditorUiThemeToken::None);
        }
        CHECK(first.Value().nodes[3].backgroundToken == EditorUiThemeToken::Surface);
        CHECK(first.Value().nodes[10].borderToken == EditorUiThemeToken::Accent);
        CHECK(first.Value().nodes[11].foregroundToken == EditorUiThemeToken::Warning);
    }

    TEST_CASE("Responsive row projection shares the same deterministic model across adapters", "[Extensions][EditorUiForm]") {
        const EditorUiForm form = BuildResponsiveForm();
        EditorUiThemeFrame theme;
        theme.revision = 9;
        theme.supportedTokenMask = EditorUiThemeTokenBit(EditorUiThemeToken::Surface) |
                                   EditorUiThemeTokenBit(EditorUiThemeToken::TextPrimary) |
                                   EditorUiThemeTokenBit(EditorUiThemeToken::TextSecondary);
        const auto narrow = BuildEditorUiRenderSnapshot(form, theme, 240.0F);
        const auto wide = BuildEditorUiRenderSnapshot(form, theme, 480.0F);
        REQUIRE(narrow.HasValue());
        REQUIRE(wide.HasValue());
        REQUIRE(narrow.Value().nodes.size() == wide.Value().nodes.size());
        CHECK(narrow.Value().nodes[1].width == Catch::Approx(120.0F));
        CHECK(narrow.Value().nodes[2].x == Catch::Approx(120.0F));
        CHECK(wide.Value().nodes[1].width == Catch::Approx(240.0F));
        CHECK(wide.Value().nodes[2].x == Catch::Approx(240.0F));
        CHECK(narrow.Value().nodes[1].borderToken == EditorUiThemeToken::TextSecondary);
        CHECK(narrow.Value().nodes[1].focusOrder == 1);
        CHECK(narrow.Value().nodes[2].focusOrder == 2);
    }

    TEST_CASE("Unavailable semantic theme roles fall back without exposing color constants", "[Extensions][EditorUiForm]") {
        EditorUiThemeFrame theme;
        theme.supportedTokenMask =
            EditorUiThemeTokenBit(EditorUiThemeToken::Surface) | EditorUiThemeTokenBit(EditorUiThemeToken::TextPrimary);
        CHECK(ResolveEditorUiThemeToken(EditorUiThemeToken::TextDisabled, theme) == EditorUiThemeToken::TextPrimary);
        CHECK(ResolveEditorUiThemeToken(EditorUiThemeToken::SurfaceSubtle, theme) == EditorUiThemeToken::Surface);
        CHECK(ResolveEditorUiThemeToken(EditorUiThemeToken::Critical, theme) == EditorUiThemeToken::TextPrimary);
        CHECK(ResolveEditorUiThemeToken(EditorUiThemeToken::None, theme) == EditorUiThemeToken::None);

        theme.supportedTokenMask = 0;
        CHECK(ResolveEditorUiThemeToken(EditorUiThemeToken::Critical, theme) == EditorUiThemeToken::None);
    }

    TEST_CASE("Theme and form bounds fail closed", "[Extensions][EditorUiForm]") {
        const EditorUiForm form = BuildReferenceForm();
        EditorUiThemeFrame theme;
        theme.revision = 0;
        const auto invalidRevision = BuildEditorUiRenderSnapshot(form, theme);
        REQUIRE(invalidRevision.HasError());
        CHECK(invalidRevision.ErrorValue().code.Value() == "editor_ui_theme_invalid");

        theme.revision = 1;
        theme.uiScale = std::numeric_limits<float>::infinity();
        const auto invalidScale = BuildEditorUiRenderSnapshot(form, theme);
        REQUIRE(invalidScale.HasError());
        CHECK(invalidScale.ErrorValue().code.Value() == "editor_ui_theme_invalid");

        EditorUiFormLimits limits;
        limits.maximumDepth = 1;
        CHECK(ValidateEditorUiForm(form, limits).HasError());
    }
}  // namespace Horo::Extensions::Tests
