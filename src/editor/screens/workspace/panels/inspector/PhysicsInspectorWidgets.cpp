#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace Horo::Editor::PhysicsInspectorWidgets {
    /** @copydoc Label */
    const char *Label(const EditorGuiContext &context, const char *key) {
        return context.localization.Get("editor", key).c_str();
    }

    /** @copydoc BeginFields */
    bool BeginFields(const char *id) {
        const float padding = Ui::ScaledLayoutValue(12);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padding, Ui::ScaledLayoutValue(8)});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Ui::ScaledLayoutValue(8), Ui::ScaledLayoutValue(8)});
        if (!ImGui::BeginChild(id, {ImGui::GetContentRegionAvail().x, 0},
                               ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AlwaysUseWindowPadding,
                               ImGuiWindowFlags_NoScrollbar)) {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            return false;
        }
        return true;
    }

    /** @copydoc EndFields */
    void EndFields() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }

    /** @copydoc Field */
    void Field(const EditorGuiContext &context, const char *key) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, Ui::ScaledLayoutValue(4)});
        ImGui::PushTextWrapPos(0);
        Ui::FieldLabel(Label(context, key), context.theme.fonts);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
    }

    /** @copydoc Choice */
    bool Choice(const EditorGuiContext &context, const char *key, const char *id, int &value, const std::span<const char *const> entries) {
        Field(context, key);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        return Ui::ComboControl(id, &value, entries.data(), static_cast<int>(entries.size()), context.theme.fonts);
    }

    /** @copydoc Scalar */
    bool Scalar(const EditorGuiContext &context, const char *key, float &value) {
        Field(context, key);
        return Ui::DrawFloatPropRow("", key, value, context.theme.fonts).changed;
    }

    /** @copydoc Vector */
    bool Vector(const EditorGuiContext &context, const char *key, Math::Vec3 &value) {
        std::array components{value.x, value.y, value.z};
        Field(context, key);
        const auto edit = Ui::DrawFloat3PropRow("", key, components, context.theme.fonts);
        if (edit.changed)
            value = {components[0], components[1], components[2]};
        return edit.changed;
    }

    /** @copydoc Pose */
    bool Pose(const EditorGuiContext &context, Runtime::AuthoredPhysicsPose &value) {
        bool changed = Vector(context, "workspace.physics.translation", value.translation);
        if (Math::Vec3 imaginary{value.rotation.x, value.rotation.y, value.rotation.z};
            Vector(context, "workspace.physics.rotation_xyz", imaginary)) {
            value.rotation.x = imaginary.x;
            value.rotation.y = imaginary.y;
            value.rotation.z = imaginary.z;
            changed = true;
        }
        return Scalar(context, "workspace.physics.rotation_w", value.rotation.w) || changed;
    }

    /** @copydoc Toggle */
    bool Toggle(const EditorGuiContext &context, const char *key, bool &value) {
        Field(context, key);
        ImGui::PushID(key);
        const bool changed = Ui::CheckboxControl("##value", &value, context.theme.fonts);
        ImGui::PopID();
        return changed;
    }

    namespace {
        /** @brief Preserve invalid text and expose it without mutating the last valid typed identity. */
        bool Text(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text) {
            if (std::ranges::find(text.bytes, '\0') == text.bytes.end()) {
                text.bytes.back() = '\0';
                text.valid = false;
            }
            Field(context, key);
            ImGui::PushID(key);
            const bool changed =
                Ui::InputTextControl("##value", text.bytes.data(), text.bytes.size(), context.theme.fonts, {.error = !text.valid});
            ImGui::PopID();
            return changed;
        }
    }  // namespace

    namespace {
        /** @brief Never scan beyond the fixed identity buffer, even for malformed foreign draft bytes. */
        std::string_view Contents(const PhysicsInspectorText &text) {
            const auto end = std::ranges::find(text.bytes, '\0');
            return {text.bytes.data(), static_cast<std::size_t>(std::distance(text.bytes.begin(), end))};
        }
    }  // namespace

    /** @copydoc Integer */
    bool Integer(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text, std::uint64_t &value) {
        if (!Text(context, key, text))
            return false;
        const auto input = Contents(text);
        const char *end = input.data() + input.size();
        std::uint64_t parsed{};
        const auto result = std::from_chars(text.bytes.data(), end, parsed);
        text.valid =
            !input.empty() && input.size() <= 20 && input.front() != '0' && result.ec == std::errc{} && result.ptr == end && parsed != 0;
        if (text.valid)
            value = parsed;
        return true;
    }

    /** @copydoc Asset */
    bool Asset(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text, Assets::AssetId &value) {
        if (!Text(context, key, text))
            return false;
        const auto parsed = Assets::AssetId::Parse(Contents(text));
        text.valid = parsed.HasValue();
        if (text.valid)
            value = parsed.Value();
        return true;
    }

    /** @copydoc Profile */
    bool Profile(const EditorGuiContext &context, PhysicsInspectorText &text, Physics::CollisionProfileId &value) {
        if (!Text(context, "workspace.physics.profile", text))
            return false;
        const auto parsed = Physics::CollisionProfileId::Parse(Contents(text));
        text.valid = parsed.HasValue();
        if (text.valid)
            value = parsed.Value();
        return true;
    }

    /** @copydoc Stable */
    void Stable(const EditorGuiContext &context, const char *key, const std::uint64_t value) {
        std::array<char, 21> text{};
        static_cast<void>(std::to_chars(text.data(), text.data() + text.size() - 1, value));
        Field(context, key);
        ImGui::PushID(key);
        ImGui::BeginDisabled();
        (void)Ui::InputTextControl("##value", text.data(), text.size(), context.theme.fonts);
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    /** @copydoc Details */
    void Details(const EditorGuiContext &context, const std::uint64_t component) {
        Ui::Card card({.id = "component_details", .defaultExpanded = false});
        card.DrawTitleBar({.id = "title", .title = Label(context, "workspace.physics.details"), .fonts = context.theme.fonts});
        if (card.BeginBody() && BeginFields()) {
            Stable(context, "workspace.physics.component_id", component);
            EndFields();
        }
        card.Finish();
    }

    /** @copydoc Action */
    bool Action(const EditorGuiContext &context, const char *key, const bool enabled, const bool primary) {
        const char *label = Label(context, key);
        const float available = ImGui::GetContentRegionAvail().x;
        const bool clicked = Ui::Button({.label = label,
                                         .variant = primary ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                                         .enabled = enabled,
                                         .font = context.theme.fonts.sans,
                                         .style = {.width = Ui::StyleWidth::FillAvailable}});
        if (ImGui::CalcTextSize(label).x > available && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
            Ui::ShowTooltip(label, &context.theme.fonts);
        return clicked;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
