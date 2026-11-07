#include "EditorInputActions.h"

#include <algorithm>
#include <cctype>

namespace Horo::Editor {
    namespace {
        Input::InputBinding KeyBinding(const Input::Key key, const bool control = false, const bool shift = false,
                                       const bool command = false) {
            return Input::InputBinding{.kind = Input::BindingControlKind::Key,
                                       .key = key,
                                       .requiredModifiers = {.control = control, .shift = shift, .command = command}};
        }

        Input::ActionDescriptor Digital(const char *id, const bool required, std::vector<Input::InputBinding> bindings) {
            return Input::ActionDescriptor{Input::ActionId{id}, Input::ActionValueType::Digital,
                                           Input::InputContextId{kEditorWorkspaceInputContext}, required, std::move(bindings)};
        }

        Input::InputBinding AxisKey(const Input::Key key, const std::uint8_t component, const float scale) {
            Input::InputBinding binding = KeyBinding(key);
            binding.component = component;
            binding.scale = scale;
            return binding;
        }
    }  // namespace

    std::vector<Input::ActionDescriptor> BuildEditorInputActions() {
        using enum Input::Key;
        return {
            Digital(kActionSave, true, {KeyBinding(S, true), KeyBinding(S, false, false, true)}),
            Digital(kActionUndo, true, {KeyBinding(Z, true), KeyBinding(Z, false, false, true)}),
            Digital(kActionRedo, true, {KeyBinding(Z, true, true), KeyBinding(Z, false, true, true)}),
            Digital(kActionSourceSelectAll, false, {KeyBinding(A, true), KeyBinding(A, false, false, true)}),
            Digital(kActionSourceCopy, false, {KeyBinding(C, true), KeyBinding(C, false, false, true)}),
            Digital(kActionSourcePaste, false, {KeyBinding(V, true), KeyBinding(V, false, false, true)}),
            Digital(kActionSourceCut, false, {KeyBinding(X, true), KeyBinding(X, false, false, true)}),
            Digital(kActionSourceFind, false, {KeyBinding(F, true), KeyBinding(F, false, false, true)}),
            Digital(kActionSourceGoToLine, false, {KeyBinding(G, true), KeyBinding(G, false, false, true)}),
            Digital(kActionSourceZoomIn, false,
                    {KeyBinding(Equal, true), KeyBinding(Equal, true, true), KeyBinding(Equal, false, false, true),
                     KeyBinding(Equal, false, true, true)}),
            Digital(kActionSourceZoomOut, false, {KeyBinding(Minus, true), KeyBinding(Minus, false, false, true)}),
            Digital(kActionSourceZoomReset, false, {KeyBinding(Digit0, true), KeyBinding(Digit0, false, false, true)}),
            Digital(kActionDuplicate, false, {KeyBinding(D, true), KeyBinding(D, false, false, true)}),
            Digital(kActionDelete, false, {KeyBinding(Delete)}),
            Digital(kActionToolSelect, true, {KeyBinding(Q)}),
            Digital(kActionToolMove, true, {KeyBinding(W)}),
            Digital(kActionToolRotate, true, {KeyBinding(E)}),
            Digital(kActionToolScale, true, {KeyBinding(R)}),
            Digital(kActionViewportFocusSelected, false, {KeyBinding(F)}),
            Input::ActionDescriptor{Input::ActionId{kGameplayMoveAction},
                                    Input::ActionValueType::Axis2D,
                                    Input::InputContextId{kEditorWorkspaceInputContext},
                                    false,
                                    {AxisKey(Left, 0, -1.0F), AxisKey(Right, 0, 1.0F), AxisKey(Down, 1, -1.0F), AxisKey(Up, 1, 1.0F)}},
        };
    }

    /** @copydoc RegisteredEditorInputActions */
    std::span<const Input::ActionDescriptor> RegisteredEditorInputActions() {
        static const auto actions = BuildEditorInputActions();
        return actions;
    }

    /** @copydoc EditorActionBindings */
    std::span<const Input::InputBinding> EditorActionBindings(const std::string_view id, const Input::InputBindingProfile &profile) {
        for (const auto &override : profile.overrides)
            if (override.action.Value() == id)
                return override.bindings;
        for (const auto &action : RegisteredEditorInputActions())
            if (action.id.Value() == id)
                return action.defaultBindings;
        return {};
    }

    /** @copydoc EditorKeymapActions */
    std::vector<Input::ActionDescriptor> EditorKeymapActions(const Input::InputBindingProfile &profile,
                                                             const std::span<const Input::ActionDescriptor> contributions) {
        auto actions = BuildEditorInputActions();
        for (const auto &action : contributions)
            if (std::ranges::none_of(actions, [&](const auto &known) {
                return known.id == action.id;
            }))
                actions.push_back(action);
        for (const auto &entry : profile.overrides)
            if (!entry.action.Value().empty() && entry.action.Value().size() <= 256 && entry.action.Value().front() != '.' &&
                entry.action.Value().back() != '.' && entry.action.Value().find("..") == std::string::npos &&
                entry.action.Value().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos &&
                !entry.action.Value().starts_with("editor.") && std::ranges::none_of(actions, [&](const auto &known) {
                return known.id == entry.action;
            }))
                actions.push_back({entry.action, Input::ActionValueType::Digital, Input::InputContextId{entry.action.Value()}, false, {}});
        return actions;
    }

    /** @copydoc BuiltinEditorInputProfile */
    Input::InputBindingProfile BuiltinEditorInputProfile(const Input::InputBindingProfile &profile) {
        auto filtered = profile;
        std::erase_if(filtered.overrides, [](const auto &entry) {
            return std::ranges::none_of(RegisteredEditorInputActions(), [&](const auto &known) {
                return known.id == entry.action;
            });
        });
        return filtered;
    }

    /** @copydoc ParseEditorShortcut */
    std::optional<Input::InputBinding> ParseEditorShortcut(const std::string_view shortcut) {
        if (shortcut.empty() || shortcut.size() > 64)
            return std::nullopt;
        Input::InputBinding binding{.kind = Input::BindingControlKind::Key};
        std::size_t start = 0;
        for (;;) {
            const auto separator = shortcut.find('+', start);
            std::string token{shortcut.substr(start, separator == std::string_view::npos ? separator : separator - start)};
            std::ranges::transform(token, token.begin(), [](const unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (separator == std::string_view::npos) {
                for (int index = 1; index < static_cast<int>(Input::Key::Count); ++index) {
                    const auto key = static_cast<Input::Key>(index);
                    const auto glyph = Input::CanonicalGlyph({Input::BindingControlKind::Key, static_cast<std::uint16_t>(key)});
                    std::string label{glyph.label};
                    std::erase(label, ' ');
                    std::ranges::transform(label, label.begin(), [](const unsigned char c) {
                        return static_cast<char>(std::toupper(c));
                    });
                    if (label == token) {
                        binding.key = key;
                        return binding;
                    }
                }
                return std::nullopt;
            }
            bool *modifier = token == "CTRL"    ? &binding.requiredModifiers.control
                             : token == "SHIFT" ? &binding.requiredModifiers.shift
                             : token == "ALT"   ? &binding.requiredModifiers.alt
                             : token == "META"  ? &binding.requiredModifiers.command
                                                : nullptr;
            if (!modifier || *modifier)
                return std::nullopt;
            *modifier = true;
            start = separator + 1;
        }
    }

}  // namespace Horo::Editor
