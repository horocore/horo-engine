#include "EditorActivityAbiConversion.h"
#include "EditorInspectorNativeSession.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>

namespace Horo::Extensions {
    namespace {
        static_assert(std::is_standard_layout_v<HoroEditorInspectorDescriptorV2>);
        static_assert(offsetof(HoroEditorInspectorDescriptorV2, base) == 0);

        /** @brief Builds typed malformed transport errors without dropping downstream form validation causes. */
        template <class T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        }

        /** @brief Copies a declared primitive and its exact constraints; placeholders are overwritten from actual owner data before draw.
         */
        Result<EditorInspectorField> CopyField(const HoroEditorInspectorField &input, EditorUiForm &form) {
            if (!HoroEditorInspectorFieldShape(&input))
                return Invalid<EditorInspectorField>();
            EditorInspectorField field;
            EditorUiNodeBase base;
            if (!Detail::CopyActivityText(input.fieldId, field.field, 96) || !Detail::CopyActivityText(input.nodeId, base.id.value) ||
                !Detail::CopyActivityText(input.parentNodeId, base.parent.value) ||
                !Detail::CopyActivityText(input.labelKey, base.label.value, 128))
                return Invalid<EditorInspectorField>();
            field.node = base.id;
            field.kind = static_cast<EditorInspectorFieldKind>(input.kind);
            field.maximumStringBytes = input.maximumStringBytes;
            base.enabled = (input.flags & HORO_EDITOR_PAGE_FIELD_DISABLED) == 0;
            base.readOnly = (input.flags & HORO_EDITOR_PAGE_FIELD_READ_ONLY) != 0;
            base.label.kind = EditorUiTextKind::LocalizationKey;
            if (field.kind == EditorInspectorFieldKind::Integer) {
                if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MINIMUM) != 0)
                    field.minimumInteger = input.minimumInteger;
                if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MAXIMUM) != 0)
                    field.maximumInteger = input.maximumInteger;
                form.nodes.emplace_back(EditorUiNumberNode{.base = base,
                                                           .binding = {field.field},
                                                           .numberKind = EditorUiNumberKind::Integer,
                                                           .value = std::int64_t{0}});
            } else if (field.kind == EditorInspectorFieldKind::Number) {
                if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MINIMUM) != 0)
                    field.minimumNumber = input.minimumNumber;
                if ((input.flags & HORO_EDITOR_PAGE_INTEGER_MAXIMUM) != 0)
                    field.maximumNumber = input.maximumNumber;
                const auto placeholder = field.minimumNumber.value_or(field.maximumNumber.value_or(0));
                form.nodes.emplace_back(EditorUiNumberNode{.base = base,
                                                           .binding = {field.field},
                                                           .value = placeholder,
                                                           .minimum = field.minimumNumber,
                                                           .maximum = field.maximumNumber});
            } else if (field.kind == EditorInspectorFieldKind::Boolean)
                form.nodes.emplace_back(EditorUiBooleanNode{base, {field.field}, false});
            else
                form.nodes.emplace_back(
                    EditorUiTextFieldNode{.base = base, .binding = {field.field}, .maximumBytes = field.maximumStringBytes});
            return Result<EditorInspectorField>::Success(std::move(field));
        }

        /** @brief Applies copied semantic styles to exact unique canonical nodes without changing their bindings. */
        Result<void> CopyPresentations(const HoroEditorInspectorDescriptor &input, EditorUiForm &form) {
            if (input.schemaVersion == HORO_EDITOR_INSPECTOR_SCHEMA_VERSION)
                return Result<void>::Success();
            const auto &extended = *static_cast<const HoroEditorInspectorDescriptorV2 *>(static_cast<const void *>(&input));
            for (std::uint32_t i = 0; i < extended.presentationCount; ++i) {
                std::string id;
                if (!Detail::CopyActivityText(extended.presentations[i].nodeId, id))
                    return Invalid<void>();
                const auto node = std::ranges::find_if(form.nodes, [&](const auto &candidate) {
                    return EditorUiNodeBaseOf(candidate).id.value == id;
                });
                if (node == form.nodes.end() || EditorUiNodeBaseOf(*node).custom)
                    return Invalid<void>();
                auto style = Detail::CopyCustomPresentation(&extended.presentations[i].presentation);
                if (style.HasError())
                    return Result<void>::Failure(style.ErrorValue());
                std::visit([&](auto &typed) {
                    typed.base.custom = std::move(style).Value();
                }, node->payload);
            }
            return Result<void>::Success();
        }

        /** @brief Copies a finite package catalog, charging total bytes before retaining its strings. */
        Result<void> CopyMessages(const HoroEditorInspectorDescriptor &input, EditorInspectorDefinition &definition) {
            std::size_t total{};
            for (std::uint32_t i = 0; i < input.messageCount; ++i) {
                const auto &source = input.messages[i];
                EditorInspectorMessage message;
                if (!Detail::CopyActivityText(source.locale, message.locale, 32) ||
                    !Detail::CopyActivityText(source.key, message.key, 128) || !Detail::CopyActivityText(source.value, message.value, 4096))
                    return Invalid<void>();
                total += message.locale.size() + message.key.size() + message.value.size();
                if (total > 65536)
                    return Invalid<void>();
                definition.messages.push_back(std::move(message));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CopyInspectorDefinition */
    Result<EditorInspectorDefinition> CopyInspectorDefinition(const HoroEditorInspectorDescriptor &input) {
        if (!HoroEditorInspectorDescriptorShape(&input))
            return Invalid<EditorInspectorDefinition>();
        EditorInspectorDefinition definition;
        definition.behaviorSchemaVersion = input.behaviorSchemaVersion;
        if (!Detail::CopyActivityText(input.behaviorType, definition.behaviorType, 160) ||
            !Detail::CopyActivityText(input.propertyField, definition.field, 96) ||
            !Detail::CopyActivityText(input.surfaceId, definition.form.id.value) ||
            !Detail::CopyActivityText(input.labelKey, definition.form.title.value, 128))
            return Invalid<EditorInspectorDefinition>();
        definition.form.title.kind = EditorUiTextKind::LocalizationKey;
        if (input.presentationNodeCount != 0) {
            const auto version = input.schemaVersion == HORO_EDITOR_INSPECTOR_SCHEMA_VERSION ? HORO_EDITOR_ACTIVITY_SCHEMA_VERSION
                                                                                             : HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION;
            const HoroEditorActivitySnapshot presentation{sizeof(presentation),       version, 1, 0, 0, input.presentationNodes,
                                                          input.presentationNodeCount};
            auto form = Detail::CopyActivityForm(presentation, definition.form.id.value, definition.form.title.value);
            if (form.HasError())
                return Result<EditorInspectorDefinition>::Failure(form.ErrorValue());
            definition.form = std::move(form).Value();
        }
        for (std::uint32_t i = 0; i < input.fieldCount; ++i) {
            auto field = CopyField(input.fields[i], definition.form);
            if (field.HasError())
                return Result<EditorInspectorDefinition>::Failure(field.ErrorValue());
            definition.fields.push_back(std::move(field).Value());
        }
        if (auto styled = CopyPresentations(input, definition.form); styled.HasError())
            return Result<EditorInspectorDefinition>::Failure(styled.ErrorValue());
        if (auto copied = CopyMessages(input, definition); copied.HasError())
            return Result<EditorInspectorDefinition>::Failure(copied.ErrorValue());
        return Result<EditorInspectorDefinition>::Success(std::move(definition));
    }
}  // namespace Horo::Extensions
