#include "editor/screens/workspace/panels/inspector/PhysicsInspectorWidgets.h"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace Horo::Editor::PhysicsInspectorWidgets {
    /** @copydoc Label */
    const char *Label(const EditorGuiContext &context, const char *key) {
        return context.localization.Get("editor", key).c_str();
    }

    /** @copydoc Scalar */
    bool Scalar(const EditorGuiContext &context, const char *key, float &value) {
        return Ui::DrawFloatPropRow(Label(context, key), key, value, context.theme.fonts).changed;
    }

    /** @copydoc Vector */
    bool Vector(const EditorGuiContext &context, const char *key, Math::Vec3 &value) {
        std::array components{value.x, value.y, value.z};
        const auto edit = Ui::DrawFloat3PropRow(Label(context, key), key, components, context.theme.fonts);
        if (edit.changed)
            value = {components[0], components[1], components[2]};
        return edit.changed;
    }

    /** @copydoc Pose */
    bool Pose(const EditorGuiContext &context, Runtime::AuthoredPhysicsPose &value) {
        bool changed = Vector(context, "workspace.physics.translation", value.translation);
        Math::Vec3 imaginary{value.rotation.x, value.rotation.y, value.rotation.z};
        if (Vector(context, "workspace.physics.rotation_xyz", imaginary)) {
            value.rotation.x = imaginary.x;
            value.rotation.y = imaginary.y;
            value.rotation.z = imaginary.z;
            changed = true;
        }
        return Scalar(context, "workspace.physics.rotation_w", value.rotation.w) || changed;
    }

    /** @copydoc Toggle */
    bool Toggle(const EditorGuiContext &context, const char *key, bool &value) {
        return Ui::CheckboxControl(Label(context, key), &value, context.theme.fonts);
    }

    namespace {
        /** @brief Preserve invalid text and expose it without mutating the last valid typed identity. */
        bool Text(const EditorGuiContext &context, const char *key, PhysicsInspectorText &text) {
            if (std::ranges::find(text.bytes, '\0') == text.bytes.end()) {
                text.bytes.back() = '\0';
                text.valid = false;
            }
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            return Ui::InputTextControl(key, text.bytes.data(), text.bytes.size(), context.theme.fonts, {.error = !text.valid});
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
        Ui::DrawPropRow(Label(context, key), text.data(), context.theme.fonts);
    }

    /** @copydoc Action */
    bool Action(const EditorGuiContext &context, const char *key, const bool enabled) {
        const char *label = Label(context, key);
        const float available = ImGui::GetContentRegionAvail().x;
        const bool clicked = Ui::Button(
            {.label = label, .enabled = enabled, .font = context.theme.fonts.sans, .style = {.width = Ui::StyleWidth::FillAvailable}});
        if (ImGui::CalcTextSize(label).x > available && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
            Ui::ShowTooltip(label, &context.theme.fonts);
        return clicked;
    }
}  // namespace Horo::Editor::PhysicsInspectorWidgets
