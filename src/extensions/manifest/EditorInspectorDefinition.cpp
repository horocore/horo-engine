#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace Horo::Extensions {
    namespace {
        /** @brief Keeps declarations and candidate rejection in the existing typed FormKit validation domain. */
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        }

        /** @brief Checks one exact primitive and the host-owned field binding; no placeholder becomes an authored value. */
        bool MatchesNode(const EditorInspectorField &field, const EditorUiNode &node) {
            return std::visit([&]<class T>(const T &typed) {
                if constexpr (std::is_same_v<T, EditorUiBooleanNode>)
                    return field.kind == EditorInspectorFieldKind::Boolean && typed.binding.value == field.field;
                else if constexpr (std::is_same_v<T, EditorUiTextFieldNode>)
                    return field.kind == EditorInspectorFieldKind::String && typed.binding.value == field.field && !typed.multiline &&
                           typed.maximumBytes == field.maximumStringBytes;
                else if constexpr (std::is_same_v<T, EditorUiNumberNode>) {
                    if (typed.binding.value != field.field || typed.step.has_value())
                        return false;
                    if (field.kind == EditorInspectorFieldKind::Integer)
                        return typed.numberKind == EditorUiNumberKind::Integer && !typed.minimum && !typed.maximum;
                    return field.kind == EditorInspectorFieldKind::Number && typed.numberKind == EditorUiNumberKind::Decimal &&
                           typed.minimum == field.minimumNumber && typed.maximum == field.maximumNumber;
                } else
                    return false;
            }, node.payload);
        }

        /** @brief Rejects unsupported actions/layout/interaction rather than hiding inert controls in an admitted inspector. */
        bool SupportedNode(const EditorUiNode &node, const std::vector<EditorInspectorField> &fields) {
            using enum EditorUiFocusPolicy;
            const auto &base = EditorUiNodeBaseOf(node);
            if (base.size != EditorUiComponentSize::Medium || base.tone != EditorUiSemanticTone::Neutral)
                return false;
            return std::visit([&]<class T>(const T &typed) {
                if constexpr (std::is_same_v<T, EditorUiContainerNode>)
                    return typed.layout == EditorUiLayoutKind::Stack || typed.layout == EditorUiLayoutKind::Group;
                else if constexpr (std::is_same_v<T, EditorUiTextNode> || std::is_same_v<T, EditorUiLabelNode>)
                    return base.focusPolicy == Automatic || base.focusPolicy == Never;
                else if constexpr (requires { typed.binding; }) {
                    const auto field = std::ranges::find(fields, base.id, &EditorInspectorField::node);
                    return base.focusPolicy == Automatic && field != fields.end() && MatchesNode(*field, node);
                } else
                    return false;
            }, node.payload);
        }

        /** @brief Rejects invalid constraints at schema admission even before a real owner supplies current values. */
        bool ValidLimits(const EditorInspectorField &field) {
            using enum EditorInspectorFieldKind;
            if (field.kind > String || field.maximumStringBytes > 4096)
                return false;
            if (field.kind != Integer && (field.minimumInteger.has_value() || field.maximumInteger.has_value()))
                return false;
            if (field.kind != Number && (field.minimumNumber.has_value() || field.maximumNumber.has_value()))
                return false;
            return (!field.minimumNumber.has_value() || std::isfinite(*field.minimumNumber)) &&
                   (!field.maximumNumber.has_value() || std::isfinite(*field.maximumNumber)) &&
                   (!field.minimumInteger.has_value() || !field.maximumInteger.has_value() ||
                    *field.minimumInteger <= *field.maximumInteger) &&
                   (!field.minimumNumber.has_value() || !field.maximumNumber.has_value() || *field.minimumNumber <= *field.maximumNumber);
        }

        /** @brief Requires the English fallback of a displayed key while allowing an omitted optional label. */
        bool KnownMessage(const EditorInspectorDefinition &definition, std::string_view key) {
            return key.empty() || std::ranges::any_of(definition.messages, [key](const auto &message) {
                return message.locale == "en-US" && message.key == key;
            });
        }

        /** @brief Resolves localized copy while retaining explicitly technical host text. */
        bool ValidText(const EditorInspectorDefinition &definition, const EditorUiText &text) {
            return text.kind == EditorUiTextKind::TechnicalText || KnownMessage(definition, text.value);
        }

        /** @brief Validates every base and payload text slot without admitting undeclared localization keys. */
        bool ValidNodeMessages(const EditorInspectorDefinition &definition, const EditorUiNode &node) {
            if (const auto &base = EditorUiNodeBaseOf(node); !ValidText(definition, base.label) ||
                                                             !ValidText(definition, base.description) ||
                                                             !ValidText(definition, base.accessibleLabel))
                return false;
            return std::visit([&definition]<class T>(const T &typed) {
                if constexpr (requires { typed.text; }) {
                    if (!ValidText(definition, typed.text))
                        return false;
                }
                if constexpr (requires { typed.placeholder; }) {
                    if (!ValidText(definition, typed.placeholder))
                        return false;
                }
                return true;
            }, node.payload);
        }

        /** @brief Checks bounded copied catalogs and required English fallback for every displayed localization key. */
        bool ValidMessages(const EditorSurfaceDescriptor &surface, const EditorInspectorDefinition &definition) {
            std::size_t bytes{};
            for (std::size_t i = 0; i < definition.messages.size(); ++i) {
                const auto &message = definition.messages[i];
                if (message.locale.empty() || message.locale.size() > 32 || message.key.empty() || message.key.size() > 128 ||
                    message.value.empty() || message.value.size() > 4096 || message.value.find('\0') != std::string::npos ||
                    !IsValidUtf8ScalarSequence(message.value))
                    return false;
                bytes += message.locale.size() + message.key.size() + message.value.size();
                if (bytes > 65536)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (message.locale == definition.messages[j].locale && message.key == definition.messages[j].key)
                        return false;
            }
            if (!KnownMessage(definition, surface.labelLocalizationKey) || !KnownMessage(definition, surface.tooltipLocalizationKey) ||
                !ValidText(definition, definition.form.title) || !ValidText(definition, definition.form.description))
                return false;
            return std::ranges::all_of(definition.form.nodes, [&definition](const auto &node) {
                return ValidNodeMessages(definition, node);
            });
        }
    }  // namespace

    /** @copydoc CanEditEditorInspectorField */
    bool CanEditEditorInspectorField(const EditorInspectorDefinition &definition, const EditorInspectorField &field) noexcept {
        auto current = std::ranges::find_if(definition.form.nodes, [&](const auto &node) {
            return EditorUiNodeBaseOf(node).id == field.node;
        });
        for (std::size_t depth = 0; depth < definition.form.nodes.size(); ++depth) {
            if (current == definition.form.nodes.end())
                return false;
            const auto &base = EditorUiNodeBaseOf(*current);
            if (!base.enabled || base.readOnly || (depth == 0 && base.focusPolicy == EditorUiFocusPolicy::Never))
                return false;
            if (!base.parent.IsValid())
                return true;
            current = std::ranges::find_if(definition.form.nodes, [&](const auto &node) {
                return EditorUiNodeBaseOf(node).id == base.parent;
            });
        }
        return false;
    }

    /** @copydoc ValidateEditorInspectorValue */
    Result<void> ValidateEditorInspectorValue(const EditorInspectorField &field, const EditorInspectorScalar &value) {
        if (!ValidLimits(field) || static_cast<std::size_t>(field.kind) != value.index())
            return Invalid();
        if (const auto *number = std::get_if<std::int64_t>(&value)) {
            if ((field.minimumInteger.has_value() && *number < *field.minimumInteger) ||
                (field.maximumInteger.has_value() && *number > *field.maximumInteger))
                return Invalid();
        }
        if (const auto *number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number) ||
                (field.minimumNumber.has_value() && (!std::isfinite(*field.minimumNumber) || *number < *field.minimumNumber)) ||
                (field.maximumNumber.has_value() && (!std::isfinite(*field.maximumNumber) || *number > *field.maximumNumber)))
                return Invalid();
        }
        if (const auto *text = std::get_if<std::string>(&value))
            if (text->size() > field.maximumStringBytes || text->find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(*text))
                return Invalid();
        return Result<void>::Success();
    }

    /** @copydoc ValidateEditorInspectorDefinition */
    Result<void> ValidateEditorInspectorDefinition(const EditorSurfaceDescriptor &surface, const EditorInspectorDefinition &definition) {
        if (auto valid = ValidateEditorSurfaceDescriptor(surface); valid.HasError())
            return valid;
        if (const bool drawer = surface.kind == EditorSurfaceKind::PropertyDrawer;
            (surface.kind != EditorSurfaceKind::Inspector && !drawer) || surface.persistence != EditorSurfacePersistence::None ||
            surface.openByDefault || definition.behaviorType.empty() || definition.behaviorType.size() > 160 ||
            definition.behaviorType.find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(definition.behaviorType) ||
            !definition.behaviorSchemaVersion || drawer != !definition.field.empty() || definition.fields.empty() ||
            definition.fields.size() > 64 || definition.messages.size() > 128 || definition.form.id.value != surface.id ||
            (drawer && (definition.fields.size() != 1 || definition.fields.front().field != definition.field)))
            return Invalid();
        if (auto valid = ValidateEditorUiForm(definition.form); valid.HasError())
            return valid;
        for (std::size_t i = 0; i < definition.fields.size(); ++i) {
            const auto &field = definition.fields[i];
            if (!ValidLimits(field) || field.field.empty() || field.field.size() > 96 || field.field.find('\0') != std::string::npos ||
                !field.node.IsValid() ||
                (field.minimumInteger.has_value() && field.maximumInteger.has_value() && *field.minimumInteger > *field.maximumInteger) ||
                (field.minimumNumber.has_value() && field.maximumNumber.has_value() && *field.minimumNumber > *field.maximumNumber))
                return Invalid();
            for (std::size_t j = 0; j < i; ++j)
                if (field.field == definition.fields[j].field || field.node == definition.fields[j].node)
                    return Invalid();
            const auto node = std::ranges::find_if(definition.form.nodes, [&](const auto &candidate) {
                return EditorUiNodeBaseOf(candidate).id == field.node;
            });
            if (node == definition.form.nodes.end() || !MatchesNode(field, *node))
                return Invalid();
        }
        if (!std::ranges::all_of(definition.form.nodes,
                                 [&](const auto &node) {
            return SupportedNode(node, definition.fields);
        }) ||
            !ValidMessages(surface, definition))
            return Invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
