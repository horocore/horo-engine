#include "Horo/Extensions/EditorUiForm.h"

#include <string>
#include <utility>

namespace {
    using Horo::Result;
    using namespace Horo::Extensions;

    EditorUiText Localized(std::string value) {
        return EditorUiText{EditorUiTextKind::LocalizationKey, std::move(value)};
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

    Result<void> AddExampleBasics(EditorUiFormBuilder &builder) {
        if (auto result = builder.AddContainer(
                EditorUiContainerNode{.base = Base("settings", {}, "examples.gui_form_basic.settings", EditorUiFocusPolicy::Never),
                                      .layout = EditorUiLayoutKind::Group,
                                      .columns = 1});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddTextField(EditorUiTextFieldNode{.base = Base("name", "settings", "examples.gui_form_basic.name"),
                                                                     .binding = EditorUiBindingId{"settings.name"},
                                                                     .value = "Example",
                                                                     .placeholder = Localized("examples.gui_form_basic.name_hint"),
                                                                     .maximumBytes = 64,
                                                                     .multiline = false});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddNumber(EditorUiNumberNode{.base = Base("samples", "settings", "examples.gui_form_basic.samples"),
                                                               .binding = EditorUiBindingId{"settings.samples"},
                                                               .numberKind = EditorUiNumberKind::Integer,
                                                               .value = std::int64_t{4},
                                                               .minimum = 1.0,
                                                               .maximum = 16.0,
                                                               .step = 1.0});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddBoolean(EditorUiBooleanNode{.base = Base("enabled", "settings", "examples.gui_form_basic.enabled"),
                                                                 .binding = EditorUiBindingId{"settings.enabled"},
                                                                 .value = true});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        return Result<void>::Success();
    }

    Result<void> AddExamplePresentation(EditorUiFormBuilder &builder) {
        if (auto result =
                builder.AddChoice(EditorUiChoiceNode{.base = Base("mode", "settings", "examples.gui_form_basic.mode"),
                                                     .binding = EditorUiBindingId{"settings.mode"},
                                                     .selected = "balanced",
                                                     .options = {{"fast", Localized("examples.gui_form_basic.mode.fast"), true},
                                                                 {"balanced", Localized("examples.gui_form_basic.mode.balanced"), true},
                                                                 {"quality", Localized("examples.gui_form_basic.mode.quality"), true}},
                                                     .allowEmpty = false});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddColor(EditorUiColorNode{.base = Base("accent", "settings", "examples.gui_form_basic.accent"),
                                                             .binding = EditorUiBindingId{"settings.accent"},
                                                             .value = EditorUiColorValue{0.25F, 0.5F, 0.9F, 1.0F},
                                                             .allowAlpha = false});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddVector(EditorUiVectorNode{.base = Base("offset", "settings", "examples.gui_form_basic.offset"),
                                                               .binding = EditorUiBindingId{"settings.offset"},
                                                               .value = EditorUiVectorValue{{0.0, 1.0, 0.0, 0.0}, 3}});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (auto result = builder.AddAction(EditorUiActionNode{.base = Base("apply", "settings", "examples.gui_form_basic.apply"),
                                                               .action = EditorUiActionId{"actions.apply"},
                                                               .actionKind = EditorUiActionKind::Primary,
                                                               .requiresConfirmation = false});
            result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        return Result<void>::Success();
    }

    Result<EditorUiForm> BuildForm() {
        auto builderResult = EditorUiFormBuilder::Create(EditorUiId{"com.horo.examples.gui-form-basic.settings"},
                                                         Localized("examples.gui_form_basic.title"));
        if (builderResult.HasError())
            return Result<EditorUiForm>::Failure(builderResult.ErrorValue());
        auto builder = std::move(builderResult).Value();
        if (const auto result = AddExampleBasics(builder); result.HasError())
            return Result<EditorUiForm>::Failure(result.ErrorValue());
        if (const auto result = AddExamplePresentation(builder); result.HasError())
            return Result<EditorUiForm>::Failure(result.ErrorValue());
        return std::move(builder).Build();
    }
}  // namespace

int main() {
    const auto form = BuildForm();
    if (form.HasError())
        return 1;

    Horo::Extensions::EditorUiThemeFrame theme;
    const auto snapshot = Horo::Extensions::BuildEditorUiRenderSnapshot(form.Value(), theme, 640.0F);
    return snapshot.HasValue() && snapshot.Value().nodes.size() == form.Value().nodes.size() ? 0 : 1;
}
