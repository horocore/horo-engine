#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace Horo::Extensions {
    namespace {
        /** @brief Keeps custom schema failures in the canonical form validation domain. */
        Result<void> Invalid(const char *reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid, reason));
        }

        /** @brief Validates roles and finite theme-relative geometry without resolving resources or invoking code. */
        bool ValidPresentation(const EditorUiCustomPresentation &style) noexcept {
            return style.schemaVersion == 1 && style.widget <= EditorUiCustomWidget::ScalarDial && style.font > EditorThemeFontRole::None &&
                   style.font < EditorThemeFontRole::Icon && style.icon < EditorThemeIconRole::Count &&
                   style.accent > EditorThemeColorRole::None && style.accent < EditorThemeColorRole::Count &&
                   style.motion < EditorThemeMotionRole::Count && style.controlRows >= 1 && style.controlRows <= 8;
        }

        /** @brief A scalar dial can only propose exact decimal edits against a finite nonempty admitted range. */
        bool ValidDial(const EditorUiNode &node) noexcept {
            const auto *number = std::get_if<EditorUiNumberNode>(&node.payload);
            return number && number->numberKind == EditorUiNumberKind::Decimal && number->minimum.has_value() &&
                   number->maximum.has_value() && *number->minimum < *number->maximum && std::isfinite(*number->maximum - *number->minimum);
        }

        /** @brief Bounds aggregate host work, independent of the concrete renderer or provider's claims. */
        struct Consumption final {
            std::size_t widgets{};
            std::size_t samples{};
            std::size_t stateBytes{};
            std::size_t segments{};
        };

        /** @brief Charges every specialized node before host state or draw buffers can be allocated. */
        void Charge(const EditorUiNode &node, Consumption &used) noexcept {
            const auto &base = EditorUiNodeBaseOf(node);
            const auto *plot = std::get_if<EditorUiPlotNode>(&node.payload);
            if (!plot && !base.custom)
                return;
            ++used.widgets;
            used.stateBytes += sizeof(EditorUiScalarState) + sizeof(std::size_t) + base.id.value.size();
            used.segments += 64;
            if (plot) {
                used.samples += plot->samples.size();
                used.stateBytes += sizeof(EditorUiPlotState);
                used.segments += plot->samples.size();
            }
        }

        /** @brief Resolves motion without adding a widget clock or overriding reduced-motion policy. */
        void Animate(float &value, bool active, const EditorUiCustomPresentation &style, const EditorThemeFrame &theme,
                     float elapsed) noexcept {
            const float duration = MotionFor(theme, style.motion);
            const float target = active ? 1.0F : 0.0F;
            if (!std::isfinite(value))
                value = 0;
            if (duration <= 0)
                value = target;
            else if (std::isfinite(elapsed) && elapsed >= 0) {
                const float step = std::min(elapsed / duration, 1.0F);
                value += std::clamp(target - value, -step, step);
            }
            value = std::clamp(value, 0.0F, 1.0F);
        }

        /** @brief Emits only committed intent; document ownership and undo stay with the caller's application command. */
        std::optional<double> CompleteEditorUiScalarGesture(const EditorUiNumberNode &node, EditorUiScalarState &state,
                                                            const EditorUiScalarInput input, const bool admitted) noexcept {
            using enum EditorUiScalarInput;
            if (!admitted || !node.base.enabled || node.base.readOnly)
                return {};
            if (input == Commit && state.editing) {
                state.editing = false;
                return state.preview;
            }
            if (input == Increment || input == Decrement) {
                const double step = (*node.maximum - *node.minimum) / 100.0;
                const double direction = input == Increment ? 1.0 : -1.0;
                const double current = std::get<double>(node.value);
                state.preview = std::clamp(current + direction * step, *node.minimum, *node.maximum);
                if (state.preview == current)
                    state.preview = std::nextafter(current, direction > 0 ? *node.maximum : *node.minimum);
                state.editing = false;
                return state.preview;
            }
            return {};
        }

        /** @brief Copies an admitted field value into the semantic tree; it confers no editing authority. */
        void AccessibleValue(EditorUiAccessibleNode &output, const EditorUiNode &node) {
            std::visit([&](const auto &typed) {
                if constexpr (requires { typed.value; }) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(typed.value)>, std::variant<std::int64_t, double>>)
                        std::visit([&](const auto &number) {
                            output.value = EditorUiValue{number};
                        }, typed.value);
                    else
                        output.value = EditorUiValue{typed.value};
                    output.role = EditorUiAccessibleRole::Field;
                } else if constexpr (requires { typed.selected; }) {
                    output.value = EditorUiValue{typed.selected};
                    output.role = EditorUiAccessibleRole::Field;
                } else if constexpr (requires { typed.action; })
                    output.role = EditorUiAccessibleRole::Action;
                else if constexpr (requires { typed.text; })
                    if (output.name.value.empty())
                        output.name = typed.text;
            }, node.payload);
        }
    }  // namespace

    /** @copydoc ValidateEditorUiCustomPresentation */
    Result<void> ValidateEditorUiCustomPresentation(const EditorUiNode &node) {
        if (const auto &base = EditorUiNodeBaseOf(node);
            base.custom &&
            (!ValidPresentation(*base.custom) || (base.custom->widget == EditorUiCustomWidget::ScalarDial && !ValidDial(node)) ||
             (base.custom->widget == EditorUiCustomWidget::Standard && !std::holds_alternative<EditorUiPlotNode>(node.payload))))
            return Invalid("Custom presentation version, roles or scalar binding is unsupported.");
        return Result<void>::Success();
    }

    /** @copydoc RequiresEditorUiComposition */
    bool RequiresEditorUiComposition(const EditorUiForm &form) noexcept {
        return std::ranges::any_of(form.nodes, [](const auto &node) {
            return std::holds_alternative<EditorUiPlotNode>(node.payload) || EditorUiNodeBaseOf(node).custom.has_value();
        });
    }

    /** @copydoc ValidateEditorUiComposition */
    Result<void> ValidateEditorUiComposition(const EditorUiForm &form, const EditorUiCompositionBudget &budget) {
        if (budget.maximumCustomWidgets > 64 || budget.maximumPlotSamples > 16384 || budget.maximumPresentationStateBytes > 65536 ||
            budget.maximumDrawSegments > 32768)
            return Invalid("Custom composition budgets exceed the hard host ceiling.");
        Consumption used;
        for (const auto &node : form.nodes) {
            if (auto valid = ValidateEditorUiCustomPresentation(node); valid.HasError())
                return valid;
            Charge(node, used);
        }
        if (used.widgets > budget.maximumCustomWidgets || used.samples > budget.maximumPlotSamples ||
            used.stateBytes > budget.maximumPresentationStateBytes || used.segments > budget.maximumDrawSegments)
            return Result<void>::Failure(
                MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Custom composition exceeds its host resource budget."));
        return Result<void>::Success();
    }

    /** @copydoc UpdateEditorUiScalar */
    std::optional<double> UpdateEditorUiScalar(const EditorUiNumberNode &node, EditorUiScalarState &state, const EditorUiScalarInput input,
                                               const double normalizedValue, const bool admitted, const EditorThemeFrame &theme,
                                               const float elapsedSeconds) noexcept {
        using enum EditorUiScalarInput;
        if (!node.base.custom || node.base.custom->widget != EditorUiCustomWidget::ScalarDial || !node.minimum.has_value() ||
            !node.maximum.has_value() || !std::holds_alternative<double>(node.value)) {
            state = {};
            return {};
        }
        const double current = std::get<double>(node.value);
        if (!admitted || !node.base.enabled || node.base.readOnly || input == Cancel) {
            state.preview = current;
            state.editing = false;
        } else if ((input == Begin || input == Preview) && std::isfinite(normalizedValue)) {
            state.preview = std::lerp(*node.minimum, *node.maximum, std::clamp(normalizedValue, 0.0, 1.0));
            state.editing = true;
        }
        Animate(state.emphasis, admitted && state.editing, *node.base.custom, theme, elapsedSeconds);
        return CompleteEditorUiScalarGesture(node, state, input, admitted);
    }

    /** @copydoc BuildEditorUiAccessibleNode */
    EditorUiAccessibleNode BuildEditorUiAccessibleNode(const EditorUiNode &node, const EditorUiRenderNode &render) {
        const auto &base = EditorUiNodeBaseOf(node);
        EditorUiAccessibleNode output{.id = base.id,
                                      .parent = base.parent,
                                      .name = base.accessibleLabel.value.empty() ? base.label : base.accessibleLabel,
                                      .description = base.description,
                                      .enabled = render.enabled,
                                      .readOnly = render.readOnly,
                                      .focusable = render.focusable,
                                      .focusOrder = render.focusOrder};
        AccessibleValue(output, node);
        if (std::holds_alternative<EditorUiContainerNode>(node.payload))
            output.role = EditorUiAccessibleRole::Group;
        if (const auto *plot = std::get_if<EditorUiPlotNode>(&node.payload)) {
            output.role = EditorUiAccessibleRole::Plot;
            output.itemCount = static_cast<std::uint32_t>(plot->samples.size());
            if (!plot->samples.empty()) {
                output.selectedIndex = 0;
                output.value = EditorUiVectorValue{.components = {plot->samples.front().x, plot->samples.front().y}, .dimension = 2};
            }
        }
        if (base.custom && base.custom->widget == EditorUiCustomWidget::ScalarDial)
            output.role = EditorUiAccessibleRole::ScalarDial;
        return output;
    }
}  // namespace Horo::Extensions
