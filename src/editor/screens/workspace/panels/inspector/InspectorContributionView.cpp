#include "InspectorContributionView.h"

#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Extensions/EditorPageDefinition.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "editor/design_system/components/EditorUiCustomResources.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <imgui.h>
#include <limits>

namespace Horo::Editor {
    using namespace Extensions;

    /** @copydoc InspectorContributionView::Text */
    const char *InspectorContributionView::Text(const Entry &entry, const EditorUiText &text, const EditorGuiContext &context) const {
        return text.kind == EditorUiTextKind::TechnicalText
                   ? text.value.c_str()
                   : host_.LocalizedText(entry.contribution, text.value, context.settings.settings.languageTag).data();
    }

    /** @copydoc InspectorContributionView::Prepare */
    bool InspectorContributionView::Prepare(Entry &entry) const {
        if (!host_.IsAvailable(entry.contribution) || !theme_.revision)
            return false;
        const float width = ImGui::GetContentRegionAvail().x;
        if (entry.render && entry.render->themeRevision == theme_.revision && entry.render->availableWidth == width)
            return true;
        auto render = BuildEditorUiRenderSnapshot(entry.form, theme_, width / theme_.uiScale);
        if (render.HasError()) {
            entry.render.reset();
            return false;
        }
        entry.render.emplace(std::move(render).Value());
        for (std::size_t i = 0; i < entry.form.nodes.size(); ++i)
            entry.order[i] = i;
        std::sort(entry.order.begin(), entry.order.begin() + static_cast<std::ptrdiff_t>(entry.form.nodes.size()), [&](auto a, auto b) {
            return entry.render->nodes[a].y != entry.render->nodes[b].y ? entry.render->nodes[a].y < entry.render->nodes[b].y : a < b;
        });
        return true;
    }

    /** @copydoc InspectorContributionView::CoveredFields */
    std::span<const std::string> InspectorContributionView::CoveredFields(Gameplay::BehaviorInstanceId attachment,
                                                                          const EditorGuiContext &) {
        const auto coverage = std::ranges::find(coverage_, attachment, &Coverage::attachment);
        if (coverage == coverage_.end())
            return {};
        for (auto &entry : entries_)
            if (entry.approved && entry.primaryAttachment == attachment && !Prepare(entry))
                return {};
        return coverage->fields;
    }

    /** @copydoc InspectorContributionView::Queue */
    void InspectorContributionView::Queue(Entry &entry, const EditorInspectorField &field, EditorInspectorScalar value,
                                          const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command) const {
        if (command.command != EditorWorkspaceViewCommand::None || !inputAdmitted_ || !entry.approved || !entry.enabled ||
            !host_.IsAvailable(entry.contribution) || !CanEditEditorInspectorField(*entry.contribution.definition, field))
            return;
        if (auto valid = ValidateEditorInspectorValue(field, value); valid.HasError()) {
            entry.failure = valid.ErrorValue();
            return;
        }
        entry.failure.reset();
        Gameplay::BehaviorFieldValue candidate = std::visit([](const auto &typed) -> Gameplay::BehaviorFieldValue {
            return typed;
        }, value);
        command.command = EditorWorkspaceViewCommand::UpdateInspectorBehaviorField;
        command.inspectorEdit = InspectorExtensionEdit{entry.contribution.context.Provider(),
                                                       entry.contribution.context.Surface().id,
                                                       entry.behavior,
                                                       entry.schemaVersion,
                                                       model.inspectorDocumentGeneration,
                                                       model.inspectorBehaviorRegistryRevision,
                                                       model.documentRevision.value,
                                                       model.selectionRevision,
                                                       entry.targets,
                                                       field.field,
                                                       std::move(candidate)};
    }

    /** @copydoc InspectorContributionView::DrawScalar */
    std::optional<EditorInspectorScalar> InspectorContributionView::DrawScalar(Entry &entry, const std::size_t index,
                                                                               const EditorInspectorField &field,
                                                                               const EditorGuiContext &context) {
        const auto *number = &std::get<EditorUiNumberNode>(entry.form.nodes[index].payload);
        const auto &render = entry.render->nodes[index];
        const auto fonts = Ui::EditorUiFonts(context.theme.fonts, render.font);
        const auto retained = std::ranges::find_if(entry.scalars, [index](const auto &candidate) {
            return candidate.first == index;
        });
        if (retained == entry.scalars.end())
            return {};
        const auto value =
            Ui::EditorUiScalar(*number, retained->second, &entry.render->accessibility[index], theme_, fonts,
                               entry.enabled && CanEditEditorInspectorField(*entry.contribution.definition, field),
                               {render.width, theme_.metrics.largeControlHeight * number->base.custom->controlRows * theme_.uiScale});
        return value ? std::optional<EditorInspectorScalar>{*value} : std::nullopt;
    }

    /** @copydoc InspectorContributionView::DrawValue */
    std::optional<EditorInspectorScalar> InspectorContributionView::DrawValue(Entry &entry, std::size_t index,
                                                                              const EditorInspectorField &field,
                                                                              const EditorGuiContext &context) {
        const auto &node = entry.form.nodes[index];
        if (const auto *number = std::get_if<EditorUiNumberNode>(&node.payload);
            number && number->base.custom && number->base.custom->widget == EditorUiCustomWidget::ScalarDial)
            return DrawScalar(entry, index, field, context);
        if (const auto *boolean = std::get_if<EditorUiBooleanNode>(&node.payload)) {
            if (auto value = boolean->value; Ui::ToggleControl("value", &value, context.theme.fonts))
                return EditorInspectorScalar{value};
            return {};
        }
        const auto capacity = field.kind == EditorInspectorFieldKind::String ? field.maximumStringBytes + 1 : std::size_t{65};
        static_cast<void>(Ui::InputTextControl("value", entry.buffers[index], capacity, context.theme.fonts));
        if (!ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsKeyPressed(ImGuiKey_Escape))
            return {};
        if (field.kind == EditorInspectorFieldKind::String)
            return EditorInspectorScalar{entry.buffers[index]};
        if (field.kind == EditorInspectorFieldKind::Integer) {
            auto parsed = ParseEditorPageInteger(entry.buffers[index]);
            if (parsed.HasValue())
                return EditorInspectorScalar{parsed.Value()};
            entry.failure = parsed.ErrorValue();
            return {};
        }
        double number{};
        const auto &text = entry.buffers[index];
        if (const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return EditorInspectorScalar{number};
        entry.failure = MakeError(ExtensionErrors::EditorUiFormInvalid);
        return {};
    }

    /** @copydoc InspectorContributionView::DrawNode */
    void InspectorContributionView::DrawNode(Entry &entry, std::size_t index, const EditorWorkspaceViewModel &model,
                                             EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        const auto &node = entry.form.nodes[index];
        const auto &base = EditorUiNodeBaseOf(node);
        ImGui::PushID(base.id.value.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + entry.render->nodes[index].width);
        const auto fonts = Ui::EditorUiFonts(context.theme.fonts, entry.render->nodes[index].font);
        if (!base.label.value.empty())
            Ui::FieldLabel(Text(entry, base.label, context), fonts);
        if (const auto field = std::ranges::find(entry.contribution.definition->fields, base.id, &EditorInspectorField::node);
            field != entry.contribution.definition->fields.end()) {
            if (entry.mixed[index])
                ImGui::TextWrapped("%s", context.localization.Get("editor", "workspace.inspector.extension_mixed").c_str());
            ImGui::BeginDisabled(!entry.enabled || !CanEditEditorInspectorField(*entry.contribution.definition, *field));
            ImGui::SetNextItemWidth(entry.render->nodes[index].width);
            if (auto value = DrawValue(entry, index, *field, context))
                Queue(entry, *field, std::move(*value), model, command);
            ImGui::EndDisabled();
        } else {
            std::visit([&](const auto &typed) {
                if constexpr (requires { typed.text; })
                    ImGui::TextWrapped("%s", Text(entry, typed.text, context));
            }, node.payload);
        }
        if (!base.description.value.empty())
            ImGui::TextWrapped("%s", Text(entry, base.description, context));
        ImGui::PopTextWrapPos();
        ImGui::PopID();
    }

    /** @copydoc InspectorContributionView::DrawApproval */
    void InspectorContributionView::DrawApproval(const Entry &entry, const EditorWorkspaceViewModel &model,
                                                 EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) const {
        if (!host_.IsAvailable(entry.contribution))
            return;
        ImGui::PushID(entry.contribution.context.Surface().id.c_str());
        ImGui::TextWrapped("%s", context.localization.Get("editor", "workspace.inspector.extension_approval").c_str());
        ImGui::TextWrapped("%s / %s", entry.contribution.context.Provider().extensionId.c_str(), entry.behavior.Value().c_str());
        for (const auto &field : entry.covered)
            ImGui::TextWrapped("%s", field.c_str());
        ImGui::BeginDisabled(command.command != EditorWorkspaceViewCommand::None);
        if (Ui::Button({.label = context.localization.Get("editor", "workspace.inspector.extension_approve").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = context.theme.fonts.sans,
                        .style = {.width = Ui::StyleWidth::FillAvailable}})) {
            command.command = EditorWorkspaceViewCommand::ApproveInspectorContribution;
            command.inspectorEdit = InspectorExtensionEdit{entry.contribution.context.Provider(),
                                                           entry.contribution.context.Surface().id,
                                                           entry.behavior,
                                                           entry.schemaVersion,
                                                           model.inspectorDocumentGeneration,
                                                           model.inspectorBehaviorRegistryRevision,
                                                           model.documentRevision.value,
                                                           model.selectionRevision,
                                                           entry.targets,
                                                           {},
                                                           {}};
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    /** @copydoc InspectorContributionView::DrawEntry */
    void InspectorContributionView::DrawEntry(Entry &entry, const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                              const EditorGuiContext &context) {
        const Ui::ScopedEditorUiInput inputScope{inputAdmitted_};
        if (!entry.approved) {
            DrawApproval(entry, model, command, context);
            return;
        }
        if (!Prepare(entry))
            return;
        ImGui::PushID(entry.contribution.context.Surface().id.c_str());
        std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> epoch{};
        const auto encoded = std::to_chars(epoch.data(), epoch.data() + epoch.size(), selectionRevision_);
        ImGui::PushID(epoch.data(), encoded.ptr);
        const auto origin = ImGui::GetCursorScreenPos();
        float extra{};
        float bottom{};
        for (std::size_t ordinal = 0; ordinal < entry.form.nodes.size(); ++ordinal) {
            const auto i = entry.order[ordinal];
            const auto &geometry = entry.render->nodes[i];
            ImGui::SetCursorScreenPos({origin.x + geometry.x, origin.y + geometry.y + extra});
            const auto before = ImGui::GetCursorScreenPos().y;
            const bool custom = RequiresEditorUiComposition(entry.form);
            if (custom)
                ImGui::PushClipRect({origin.x + geometry.x, origin.y + geometry.y},
                                    {origin.x + geometry.x + geometry.width, origin.y + geometry.y + geometry.height}, true);
            DrawNode(entry, i, model, command, context);
            if (custom)
                ImGui::PopClipRect();
            else
                extra += std::max(0.0F, ImGui::GetCursorScreenPos().y - before - geometry.height);
            bottom = std::max(bottom, geometry.y + geometry.height + extra);
        }
        ImGui::SetCursorScreenPos({origin.x, origin.y + bottom});
        if (entry.failure)
            ImGui::TextWrapped("%s", context.localization.Get("editor", "workspace.inspector.extension_invalid").c_str());
        ImGui::PopID();
        ImGui::PopID();
    }

    /** @copydoc InspectorContributionView::SetInputAdmitted */
    void InspectorContributionView::SetInputAdmitted(const bool admitted) noexcept {
        inputAdmitted_ = admitted;
        for (auto &entry : entries_)
            if (entry.render)
                for (std::size_t i = 0; i < entry.render->nodes.size(); ++i) {
                    entry.render->accessibility[i].enabled = admitted && entry.render->nodes[i].enabled;
                    entry.render->accessibility[i].focusable = admitted && entry.render->nodes[i].focusable;
                    if (!admitted)
                        entry.render->accessibility[i].preview = false;
                }
        if (!admitted)
            for (auto &entry : entries_)
                for (auto &[index, state] : entry.scalars) {
                    state = {};
                    if (const auto *number = std::get_if<EditorUiNumberNode>(&entry.form.nodes[index].payload))
                        state.preview = std::get<double>(number->value);
                }
    }

    /** @copydoc InspectorContributionView::Accessibility */
    std::span<const EditorUiAccessibleNode> InspectorContributionView::Accessibility(std::string_view surface) const {
        for (const auto &entry : entries_)
            if (entry.render && entry.approved && host_.IsAvailable(entry.contribution) &&
                entry.contribution.context.Surface().id == surface)
                return entry.render->accessibility;
        return {};
    }

    /** @copydoc InspectorContributionView::Draw */
    void InspectorContributionView::Draw(Gameplay::BehaviorInstanceId attachment, const EditorWorkspaceViewModel &model,
                                         EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        for (auto &entry : entries_)
            if (entry.primaryAttachment == attachment)
                DrawEntry(entry, model, command, context);
    }

    /** @copydoc InspectorContributionView::DrawMulti */
    void InspectorContributionView::DrawMulti(const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                              const EditorGuiContext &context) {
        for (auto &entry : entries_)
            DrawEntry(entry, model, command, context);
    }
}  // namespace Horo::Editor
