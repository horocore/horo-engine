#include "Horo/Extensions/EditorPageDefinition.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <charconv>
#include <ranges>
#include <type_traits>

namespace Horo::Extensions {
    namespace {
        /** @brief Keeps page-schema failures in the existing restricted form validation domain. */
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        }

        /** @brief Restricts persistent authority to an explicitly selected canonical settings domain. */
        bool PersistentScope(SettingScope scope) noexcept {
            using enum SettingScope;
            return scope == User || scope == Project || scope == Workspace;
        }

        /** @brief Matches each editable primitive to exactly one canonical field without lossy type adaptation. */
        bool MatchesNode(const EditorUiNode &node, const EditorPageField &field) {
            return std::visit([&]<typename T>(const T &typed) {
                if constexpr (!requires { typed.binding; })
                    return false;
                else if constexpr (std::is_same_v<T, EditorUiBooleanNode>)
                    return typed.binding == field.binding && field.setting.type == SettingValueType::Boolean;
                else if constexpr (std::is_same_v<T, EditorUiNumberNode>)
                    return typed.binding == field.binding && field.setting.type == SettingValueType::Integer &&
                           typed.numberKind == EditorUiNumberKind::Integer && std::holds_alternative<std::int64_t>(typed.value) &&
                           !typed.minimum.has_value() && !typed.maximum.has_value() && !typed.step.has_value();
                else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>)
                    return typed.binding == field.binding && field.setting.type == SettingValueType::String &&
                           typed.maximumBytes == field.maximumStringBytes;
                else
                    return false;
            }, node.payload);
        }

        /** @brief Advertises the implemented page transport/render profile rather than silently dropping other FormKit semantics. */
        bool SupportedProfile(const EditorUiForm &form) {
            return std::ranges::all_of(form.nodes, [](const auto &node) {
                const auto &base = EditorUiNodeBaseOf(node);
                if (base.size != EditorUiComponentSize::Medium || base.tone != EditorUiSemanticTone::Neutral)
                    return false;
                return std::visit([&]<typename T>(const T &typed) {
                    if constexpr (requires { typed.binding; } || requires { typed.action; })
                        if (base.focusPolicy != EditorUiFocusPolicy::Automatic)
                            return false;
                    if constexpr (std::is_same_v<T, EditorUiContainerNode>)
                        return typed.layout == EditorUiLayoutKind::Stack || typed.layout == EditorUiLayoutKind::Group;
                    else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>)
                        return !typed.multiline;
                    else if constexpr (std::is_same_v<T, EditorUiActionNode>)
                        return !typed.requiresConfirmation;
                    else
                        return std::is_same_v<T, EditorUiTextNode> || std::is_same_v<T, EditorUiLabelNode> ||
                               std::is_same_v<T, EditorUiBooleanNode> || std::is_same_v<T, EditorUiNumberNode>;
                }, node.payload);
            });
        }

        /** @brief Rejects duplicate aliases or a schema field that is not represented by exactly one admitted form primitive. */
        bool FieldsMatch(const EditorPageDefinition &definition) {
            for (std::size_t i = 0; i < definition.fields.size(); ++i) {
                const auto &field = definition.fields[i];
                if (!field.binding.IsValid() || field.binding.value.size() > 128 || field.setting.key.Value() != field.binding.value ||
                    field.setting.sensitivity != SettingSensitivity::Public ||
                    ValidateEditorPageValue(field, field.setting.defaultValue).HasError())
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (definition.fields[j].binding == field.binding)
                        return false;
                if (std::ranges::count_if(definition.form.nodes, [&](const auto &node) {
                    return MatchesNode(node, field);
                }) != 1)
                    return false;
            }
            return std::ranges::all_of(definition.form.nodes, [&](const auto &node) {
                return std::visit([&](const auto &typed) {
                    if constexpr (requires { typed.binding; })
                        return std::ranges::any_of(definition.fields, [&](const auto &field) {
                            return MatchesNode(node, field);
                        });
                    else
                        return true;
                }, node.payload);
            });
        }

        /** @brief Validates bounded UTF-8 messages and rejects duplicate locale/key identities before resolving references. */
        bool MessageDefinitionsValid(const EditorPageDefinition &definition) {
            if (definition.messages.size() > 128)
                return false;
            for (std::size_t i = 0; i < definition.messages.size(); ++i) {
                const auto &m = definition.messages[i];
                if (m.locale.empty() || m.locale.size() > 32 || m.key.empty() || m.key.size() > 256 || m.value.empty() ||
                    m.value.size() > 4096 || m.value.find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(m.value))
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (definition.messages[j].locale == m.locale && definition.messages[j].key == m.key)
                        return false;
            }
            return true;
        }

        /** @brief Resolves localized references against the required English fallback without inventing a message. */
        bool KnownMessage(const EditorPageDefinition &definition, std::string_view key) {
            return key.empty() || std::ranges::any_of(definition.messages, [&](const auto &message) {
                return message.locale == "en-US" && message.key == key;
            });
        }

        /** @brief Technical text is self-contained; localized text requires an admitted English reference. */
        bool KnownText(const EditorPageDefinition &definition, const EditorUiText &text) {
            return text.kind == EditorUiTextKind::TechnicalText || KnownMessage(definition, text.value);
        }

        /** @brief Checks the optional text-bearing fields of one typed form payload. */
        template <typename T> bool PayloadMessagesMatch(const EditorPageDefinition &definition, const T &typed) {
            if constexpr (requires { typed.text; })
                if (!KnownText(definition, typed.text))
                    return false;
            if constexpr (requires { typed.message; })
                if (!KnownText(definition, typed.message))
                    return false;
            if constexpr (requires { typed.placeholder; })
                if (!KnownText(definition, typed.placeholder))
                    return false;
            if constexpr (requires { typed.options; })
                return std::ranges::all_of(typed.options, [&](const auto &choice) {
                    return KnownText(definition, choice.label);
                });
            return true;
        }

        /** @brief Validates common node text before its typed payload references. */
        bool NodeMessagesMatch(const EditorPageDefinition &definition, const EditorUiNode &node) {
            if (const auto &base = EditorUiNodeBaseOf(node); !KnownText(definition, base.label) ||
                                                             !KnownText(definition, base.description) ||
                                                             !KnownText(definition, base.accessibleLabel))
                return false;
            return std::visit([&](const auto &typed) {
                return PayloadMessagesMatch(definition, typed);
            }, node.payload);
        }

        /** @brief Requires deterministic unique package messages and an en-US definition for every localized form string. */
        bool MessagesMatch(const EditorPageDefinition &definition, const EditorSurfaceDescriptor &surface) {
            if (!MessageDefinitionsValid(definition) || !KnownMessage(definition, surface.labelLocalizationKey) ||
                !KnownMessage(definition, surface.tooltipLocalizationKey) || !KnownMessage(definition, definition.form.title.value) ||
                !KnownText(definition, definition.form.description))
                return false;
            return std::ranges::all_of(definition.form.nodes, [&](const auto &node) {
                return NodeMessagesMatch(definition, node);
            });
        }

        /** @brief Checks each bounded choice's encoding and uniqueness without altering the candidate. */
        bool ChoicesValid(const EditorPageField &field) {
            for (std::size_t i = 0; i < field.choices.size(); ++i) {
                const auto &choice = field.choices[i];
                if (choice.size() > field.maximumStringBytes || choice.find('\0') != std::string::npos ||
                    !IsValidUtf8ScalarSequence(choice))
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (field.choices[j] == choice)
                        return false;
            }
            return true;
        }

        /** @brief Preserves signed integer constraints and the original rejection of choices on numeric values. */
        bool IntegerValueValid(const EditorPageField &field, const SettingValue &value) {
            if (const auto *number = std::get_if<std::int64_t>(&value))
                return (!field.minimum.has_value() || *number >= *field.minimum) &&
                       (!field.maximum.has_value() || *number <= *field.maximum) && field.choices.empty();
            return !field.minimum.has_value() && !field.maximum.has_value();
        }

        /** @brief Validates string bounds and choice membership with the same exact UTF-8 representation. */
        bool StringValueValid(const EditorPageField &field, const SettingValue &value) {
            if (const auto *text = std::get_if<std::string>(&value))
                return text->size() <= field.maximumStringBytes && text->find('\0') == std::string::npos &&
                       IsValidUtf8ScalarSequence(*text) &&
                       (field.choices.empty() || std::ranges::find(field.choices, *text) != field.choices.end()) && ChoicesValid(field);
            return field.choices.empty();
        }
    }  // namespace

    /** @copydoc ValidateEditorPageValue */
    Result<void> ValidateEditorPageValue(const EditorPageField &field, const SettingValue &value) {
        if (!ConfigurationSchema::MatchesType(field.setting.type, value) || field.maximumStringBytes > 4096 || field.choices.size() > 64 ||
            (field.minimum.has_value() && field.maximum.has_value() && *field.minimum > *field.maximum))
            return Invalid();
        if (!IntegerValueValid(field, value) || !StringValueValid(field, value))
            return Invalid();
        return Result<void>::Success();
    }

    /** @copydoc ValidateEditorPageDefinition */
    Result<void> ValidateEditorPageDefinition(const EditorSurfaceDescriptor &surface, const EditorPageDefinition &definition) {
        using enum EditorSurfaceKind;
        const bool settings = surface.kind == SettingsPage;
        const bool modal = surface.kind == Modal || surface.kind == ModalPage;
        if ((surface.kind == ModalPage) != !surface.placement.target.empty() || (!settings && !modal) ||
            surface.persistence != EditorSurfacePersistence::None || surface.openByDefault ||
            definition.schemaVersion != EditorPageSchemaVersion || definition.fields.size() > MaximumEditorPageFields)
            return Invalid();
        if (const auto valid = ValidateEditorSurfaceDescriptor(surface); valid.HasError())
            return valid;
        if (const auto valid = ValidateEditorUiForm(definition.form); valid.HasError())
            return valid;
        ConfigurationSchema schema;
        for (const auto &field : definition.fields) {
            if ((settings && !PersistentScope(field.setting.scope)) || (modal && field.setting.scope != SettingScope::Session) ||
                field.setting.scope != definition.fields.front().setting.scope)
                return Invalid();
            if (const auto valid = schema.Register(field.setting); valid.HasError())
                return valid;
        }
        if (!SupportedProfile(definition.form) || !FieldsMatch(definition) || !MessagesMatch(definition, surface))
            return Invalid();
        return Result<void>::Success();
    }

    /** @copydoc ParseEditorPageInteger */
    Result<std::int64_t> ParseEditorPageInteger(std::string_view text) {
        if (text.empty() || text.size() > 20)
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        std::int64_t value{};
        if (const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        return Result<std::int64_t>::Success(value);
    }
}  // namespace Horo::Extensions
