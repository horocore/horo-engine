#include "EditorPageAbiConversion.h"

#include "EditorActivityAbiConversion.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>

namespace Horo::Extensions::Detail {
    namespace {
        /** @brief Rejects malformed ABI input before it can allocate or publish host state. */
        template <class T = void> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        }

        /** @brief Maps only the four explicit uniform page domains; no process or path authority crosses the ABI. */
        SettingScope Scope(uint32_t scope) {
            using enum SettingScope;
            switch (scope) {
                case HORO_EDITOR_PAGE_USER:
                    return User;
                case HORO_EDITOR_PAGE_PROJECT:
                    return Project;
                case HORO_EDITOR_PAGE_WORKSPACE:
                    return Workspace;
                default:
                    return Session;
            }
        }

        /** @brief Copies one canonical full-width default, without floating-point conversion or coercion. */
        Result<SettingValue> Value(const HoroEditorPageValue &input) {
            if (!HoroEditorPageValueShape(&input))
                return Invalid<SettingValue>();
            switch (input.kind) {
                case HORO_EDITOR_PAGE_BOOLEAN:
                    if (input.booleanValue > 1)
                        return Invalid<SettingValue>();
                    return Result<SettingValue>::Success(input.booleanValue != 0);
                case HORO_EDITOR_PAGE_INTEGER:
                    return Result<SettingValue>::Success(input.integerValue);
                case HORO_EDITOR_PAGE_STRING: {
                    std::string copied;
                    if (!CopyActivityText(input.stringValue, copied, 4096))
                        return Invalid<SettingValue>();
                    return Result<SettingValue>::Success(std::move(copied));
                }
                default:
                    return Invalid<SettingValue>();
            }
        }

        /** @brief Maps the already validated ABI value kind to its canonical setting type. */
        SettingValueType ValueType(uint32_t kind) {
            using enum SettingValueType;
            switch (kind) {
                case HORO_EDITOR_PAGE_BOOLEAN:
                    return Boolean;
                case HORO_EDITOR_PAGE_INTEGER:
                    return Integer;
                default:
                    return String;
            }
        }

        /** @brief Couples a canonical setting descriptor to one standard editable primitive, using the same binding identity. */
        Result<EditorPageField> Field(const HoroEditorPageField &input, SettingScope scope, EditorUiForm &form) {
            if (!HoroEditorPageFieldShape(&input))
                return Invalid<EditorPageField>();
            EditorPageField field;
            EditorUiNodeBase base;
            if (!CopyActivityText(input.defaultValue.binding, field.binding.value) || !CopyActivityText(input.nodeId, base.id.value) ||
                !CopyActivityText(input.labelKey, base.label.value, 128))
                return Invalid<EditorPageField>();
            auto value = Value(input.defaultValue);
            if (value.HasError())
                return Result<EditorPageField>::Failure(value.ErrorValue());
            base.label.kind = EditorUiTextKind::LocalizationKey;
            base.readOnly = (input.flags & HORO_EDITOR_PAGE_FIELD_READ_ONLY) != 0;
            base.enabled = (input.flags & HORO_EDITOR_PAGE_FIELD_DISABLED) == 0;
            const auto type = ValueType(input.defaultValue.kind);
            field.setting = {SettingKey{field.binding.value}, type, std::move(value).Value(), scope, ReloadPolicy::NextOperation,
                             SettingSensitivity::Public,      {}};
            field.maximumStringBytes = input.maximumStringBytes;
            if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MINIMUM) != 0)
                field.minimum = input.minimumInteger;
            if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MAXIMUM) != 0)
                field.maximum = input.maximumInteger;
            if (type == SettingValueType::Boolean)
                form.nodes.emplace_back(EditorUiBooleanNode{base, field.binding, std::get<bool>(field.setting.defaultValue)});
            else if (type == SettingValueType::Integer)
                form.nodes.emplace_back(EditorUiNumberNode{.base = base,
                                                           .binding = field.binding,
                                                           .numberKind = EditorUiNumberKind::Integer,
                                                           .value = std::get<int64_t>(field.setting.defaultValue)});
            else
                form.nodes.emplace_back(EditorUiTextFieldNode{.base = base,
                                                              .binding = field.binding,
                                                              .value = std::get<std::string>(field.setting.defaultValue),
                                                              .maximumBytes = field.maximumStringBytes});
            return Result<EditorPageField>::Success(std::move(field));
        }

        /** @brief Copies the bounded package catalog; full duplicate/fallback validation remains the shared definition owner's
         * responsibility. */
        Result<void> Messages(const HoroEditorPageDescriptor &input, EditorPageDefinition &definition) {
            if (input.messageCount > 128 || (input.messageCount && !input.messages))
                return Invalid();
            std::size_t total{};
            for (uint32_t i = 0; i < input.messageCount; ++i) {
                EditorPageMessage message;
                if (const auto &source = input.messages[i]; !CopyActivityText(source.locale, message.locale, 32) ||
                                                            !CopyActivityText(source.key, message.key, 128) ||
                                                            !CopyActivityText(source.value, message.value, 4096))
                    return Invalid();
                total += message.locale.size() + message.key.size() + message.value.size();
                if (total > 65536)
                    return Invalid();
                definition.messages.push_back(std::move(message));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CopyPageDefinition */
    Result<EditorPageDefinition> CopyPageDefinition(const HoroEditorPageDescriptor &input, std::string_view surfaceId,
                                                    std::string_view titleKey) {
        if (!HoroEditorPageDescriptorShape(&input))
            return Invalid<EditorPageDefinition>();
        EditorPageDefinition definition;
        if (input.presentationNodeCount == 0) {
            // Editable fields are appended below before the complete page form is validated by its owner.
            definition.form.id = {std::string{surfaceId}};
            definition.form.title = {EditorUiTextKind::LocalizationKey, std::string{titleKey}};
        } else {
            HoroEditorActivitySnapshot presentation{sizeof(presentation),    HORO_EDITOR_ACTIVITY_SCHEMA_VERSION, 1, 0, 0,
                                                    input.presentationNodes, input.presentationNodeCount};
            auto form = CopyActivityForm(presentation, surfaceId, titleKey);
            if (form.HasError())
                return Result<EditorPageDefinition>::Failure(form.ErrorValue());
            definition.form = std::move(form).Value();
        }
        for (uint32_t i = 0; i < input.fieldCount; ++i) {
            auto field = Field(input.fields[i], Scope(input.scope), definition.form);
            if (field.HasError())
                return Result<EditorPageDefinition>::Failure(field.ErrorValue());
            definition.fields.push_back(std::move(field).Value());
        }
        if (auto copied = Messages(input, definition); copied.HasError())
            return Result<EditorPageDefinition>::Failure(copied.ErrorValue());
        return Result<EditorPageDefinition>::Success(std::move(definition));
    }

    /** @copydoc CopyPagePermissions */
    Result<void> CopyPagePermissions(const HoroEditorPageDescriptor &input, EditorSurfaceContextDescriptor &context) {
        if (input.permissionCount > 32 || (input.permissionCount && !input.permissions) || input.diagnosticCodeCount > 32 ||
            (input.diagnosticCodeCount && !input.diagnosticCodes))
            return Invalid();
        for (uint32_t i = 0; i < input.permissionCount; ++i) {
            std::string permission;
            if (!CopyActivityText(input.permissions[i], permission, 128))
                return Invalid();
            context.surface.requiredPermissions.emplace_back(std::move(permission));
        }
        for (uint32_t i = 0; i < input.diagnosticCodeCount; ++i) {
            std::string code;
            if (!CopyActivityText(input.diagnosticCodes[i], code, 128))
                return Invalid();
            context.diagnostics.emplace_back(std::move(code));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions::Detail
