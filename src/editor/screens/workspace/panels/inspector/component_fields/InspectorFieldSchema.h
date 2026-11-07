#pragma once
/** @file InspectorFieldSchema.h
 * @brief Target-private, typed presentation metadata; authored values remain in their owning component model.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Math/SceneMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace Horo::Editor::InspectorFields {
    /** @brief Shared primitive control vocabulary; composite transactions retain their typed adapters. */
    enum class Control {
        Number,
        Integer,
        Unsigned,
        Boolean,
        Choice,
        Text,
        Vector3,
        Color3
    };
    /** @brief Property-row layout or a wrapped label above a full-width control. */
    enum class Layout {
        PropertyRow,
        Stacked
    };
    /** @brief Exact default snapshot for tooling/tests; enums are signed choice values, never float indices. */
    using DefaultValue = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, float, Math::Vec3, std::string>;

    /** @brief Localized choice bound to an explicit enum value rather than its array position. */
    struct Choice {
        std::int64_t value;
        const char *labelKey;
    };

    /** @brief Binds a localized option to its typed enum value without relying on declaration order. */
    template <typename Enum> constexpr Choice Option(Enum value, const char *labelKey) {
        static_assert(std::is_enum_v<Enum>);
        return {static_cast<std::int64_t>(value), labelKey};
    }

    /** @brief Inert field metadata borrowing only static strings, choice arrays and host code.
     * @tparam Model Canonical typed component or detached presentation draft.
     * @details Drawing edits only Model. No field owns a document, registry, service, backend or module lease.
     */
    template <typename Model> struct Field {
        const char *id;
        const char *labelKey;
        Control control;
        Ui::PropertyEditResult (*draw)(const Field &, Model &, const EditorGuiContext &);
        DefaultValue (*defaultValue)();
        bool (*visible)(const Model &){};
        bool (*valid)(const Model &){};
        Ui::FloatPropertyOptions number{};
        float displayScale{1.0F};
        std::span<const Choice> choices{};
        bool readOnly{};
        std::size_t maximumTextBytes{4096};

        /** @brief Returns metadata with explicit numeric display units/format; model values remain canonical. */
        constexpr Field Number(Ui::FloatPropertyOptions options, float scale = 1.0F) const {
            auto copy = *this;
            copy.number = options;
            copy.displayScale = scale;
            return copy;
        }

        /** @brief Returns metadata with a current-model visibility predicate. */
        constexpr Field Visible(bool (*predicate)(const Model &)) const {
            auto copy = *this;
            copy.visible = predicate;
            return copy;
        }

        /** @brief Returns metadata with a presentation-only validation predicate; command validation remains authoritative. */
        constexpr Field Valid(bool (*predicate)(const Model &)) const {
            auto copy = *this;
            copy.valid = predicate;
            return copy;
        }

        /** @brief Returns metadata borrowing an immutable, static, explicit enum choice list. */
        constexpr Field Options(std::span<const Choice> entries) const {
            auto copy = *this;
            copy.choices = entries;
            return copy;
        }

        /** @brief Returns an immutable field rendered with a disabled shared input control. */
        constexpr Field ReadOnly() const {
            auto copy = *this;
            copy.readOnly = true;
            return copy;
        }
    };

    /** @brief Typed registry specialization point. Define one static field array per supported component model. */
    template <typename Model> struct Schema;

    /** @brief Validates static metadata without invoking accessors, predicates or drawing code.
     * @param fields Static definitions in display order. @return The same array; malformed definitions fail compilation.
     */
    template <typename Model, std::size_t N> consteval auto Checked(std::array<Field<Model>, N> fields) {
        for (std::size_t i = 0; i < N; ++i) {
            const auto &field = fields[i];
            if (!field.id || !*field.id || !field.labelKey || !*field.labelKey || !field.draw || !field.defaultValue ||
                !(field.displayScale > 0.0F) || field.displayScale > std::numeric_limits<float>::max())
                throw "Invalid Inspector field metadata";
            for (std::size_t j = 0; j < i; ++j)
                if (std::string_view{fields[j].id} == field.id)
                    throw "Duplicate Inspector field ID";
            if (field.control == Control::Choice && (field.choices.empty() || field.choices.size() > 32))
                throw "Empty Inspector choice list";
            for (std::size_t a = 0; a < field.choices.size(); ++a) {
                if (!field.choices[a].labelKey || !*field.choices[a].labelKey)
                    throw "Missing Inspector choice label";
                for (std::size_t b = 0; b < a; ++b)
                    if (field.choices[a].value == field.choices[b].value)
                        throw "Duplicate Inspector choice value";
            }
        }
        return fields;
    }

    /** @brief Draws every visible field, combining events without short-circuiting later controls.
     * @param model Mutable detached values. @param fields Static registered fields. @param context Current theme/localization.
     * @param layout Label placement. @return Per-frame change and interaction-commit events.
     */
    template <typename Model>
    [[nodiscard]] Ui::PropertyEditResult Draw(Model &model, std::span<const Field<Model>> fields, const EditorGuiContext &context,
                                              Layout layout = Layout::PropertyRow) {
        Ui::PropertyEditResult result;
        for (const auto &field : fields) {
            if (field.visible && !field.visible(model))
                continue;
            ImGui::BeginDisabled(field.readOnly);
            Field<Model> presentation = field;
            if (layout == Layout::Stacked) {
                ImGui::PushTextWrapPos(0.0F);
                Ui::FieldLabel(context.localization.Get("editor", field.labelKey).c_str(), context.theme.fonts);
                ImGui::PopTextWrapPos();
                presentation.labelKey = nullptr;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            }
            const auto edit = presentation.draw(presentation, model, context);
            ImGui::EndDisabled();
            if (!field.readOnly) {
                result.changed |= edit.changed;
                result.committed |= edit.committed;
            }
        }
        return result;
    }

    /** @brief Draws the single typed registry entry for Model. @return Combined draft interaction events. */
    template <typename Model>
    [[nodiscard]] Ui::PropertyEditResult Draw(Model &model, const EditorGuiContext &context, Layout layout = Layout::PropertyRow) {
        return Draw(model, std::span<const Field<Model>>{Schema<Model>::fields}, context, layout);
    }

    namespace Detail {
        /** @brief Reads a canonical default on demand; metadata never duplicates runtime initializers. */
        template <typename Model, auto Access> DefaultValue Default() {
            Model model{};
            const auto &value = std::invoke(Access, model);
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_enum_v<Value>)
                return static_cast<std::int64_t>(value);
            else if constexpr (std::is_same_v<Value, bool> || std::is_same_v<Value, float> || std::is_same_v<Value, Math::Vec3> ||
                               std::is_same_v<Value, std::string>)
                return value;
            else if constexpr (std::is_unsigned_v<Value>)
                return static_cast<std::uint64_t>(value);
            else
                return static_cast<std::int64_t>(value);
        }

        /** @brief Resolves field copy only at draw time, preserving locale changes without catalog rebuilds. */
        template <typename Model> const char *Label(const Field<Model> &field, const EditorGuiContext &context) {
            return field.labelKey ? context.localization.Get("editor", field.labelKey).c_str() : "";
        }

        /** @brief Shared typed adapter for ordinary scalar/vector/choice fields. */
        template <typename Model, auto Access, Control Kind>
        Ui::PropertyEditResult Draw(const Field<Model> &field, Model &model, const EditorGuiContext &context) {
            auto &value = std::invoke(Access, model);
            const char *label = Label(field, context);
            const bool valid = !field.valid || field.valid(model);
            if constexpr (Kind == Control::Number) {
                static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, float>);
                float displayed = value * field.displayScale;
                auto options = field.number;
                options.error = !valid || !std::isfinite(displayed);
                const auto edit = Ui::DrawFloatPropRow(label, field.id, displayed, context.theme.fonts, options);
                if (edit.changed)
                    value = displayed / field.displayScale;
                return edit;
            } else if constexpr (Kind == Control::Boolean) {
                const std::array entries{context.localization.Get("editor", "workspace.value.off").c_str(),
                                         context.localization.Get("editor", "workspace.value.on").c_str()};
                int selected = value ? 1 : 0;
                const bool changed = Ui::DrawComboPropRow(label, field.id, selected, entries, context.theme.fonts);
                if (changed)
                    value = selected != 0;
                return {changed, changed};
            } else if constexpr (Kind == Control::Text) {
                if (*label)
                    Ui::DrawPropRow(label, "", context.theme.fonts);
                const bool changed = Ui::InputTextControl(field.id, value, field.maximumTextBytes, context.theme.fonts, {.error = !valid});
                return {changed, ImGui::IsItemDeactivatedAfterEdit()};
            } else if constexpr (Kind == Control::Integer) {
                return Ui::DrawIntegerPropRow(label, field.id, value, context.theme.fonts);
            } else if constexpr (Kind == Control::Unsigned) {
                using Value = std::remove_cvref_t<decltype(value)>;
                std::uint64_t displayed = value;
                const auto edit = Ui::DrawUnsignedPropRow(label, field.id, displayed, context.theme.fonts, !valid);
                if (edit.changed)
                    value = static_cast<Value>(std::min(displayed, static_cast<std::uint64_t>(std::numeric_limits<Value>::max())));
                return edit;
            } else if constexpr (Kind == Control::Choice) {
                std::array<const char *, 32> labels{};
                if (field.choices.empty() || field.choices.size() > labels.size())
                    return {};
                int selected = -1;
                for (std::size_t i = 0; i < field.choices.size(); ++i) {
                    labels[i] = context.localization.Get("editor", field.choices[i].labelKey).c_str();
                    if (static_cast<std::int64_t>(value) == field.choices[i].value)
                        selected = static_cast<int>(i);
                }
                const bool changed =
                    Ui::DrawComboPropRow(label, field.id, selected, std::span<const char *const>{labels.data(), field.choices.size()},
                                         context.theme.fonts);
                if (changed && selected >= 0 && static_cast<std::size_t>(selected) < field.choices.size())
                    value = static_cast<std::remove_cvref_t<decltype(value)>>(field.choices[static_cast<std::size_t>(selected)].value);
                return {changed, changed};
            } else {
                std::array components{value.x, value.y, value.z};
                Ui::PropertyEditResult edit;
                if constexpr (Kind == Control::Color3)
                    edit = Ui::DrawColor3PropRow(label, field.id, components, context.theme.fonts, !valid);
                else
                    edit = Ui::DrawFloat3PropRow(label, field.id, components, context.theme.fonts, field.number.speed);
                if (edit.changed)
                    value = {components[0], components[1], components[2]};
                return edit;
            }
        }
    }  // namespace Detail

    /** @brief Creates an inert field from a compile-time typed member/accessor and explicit control kind.
     * @param id Stable control ID. @param labelKey Editor localization key. @return Static field; defaults come from Model{}.
     */
    template <typename Model, auto Access, Control Kind> constexpr Field<Model> Bind(const char *id, const char *labelKey) {
        return {id, labelKey, Kind, &Detail::Draw<Model, Access, Kind>, &Detail::Default<Model, Access>};
    }
}  // namespace Horo::Editor::InspectorFields
