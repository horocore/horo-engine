#include "EditorActivityAbiConversion.h"

#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::Detail {
    /** @copydoc CopyActivityText */
    bool CopyActivityText(const HoroExtensionStringView input, std::string &output, const std::size_t maximum) {
        if (input.length > maximum || (input.length != 0 && input.data == nullptr))
            return false;
        output = input.length == 0 ? std::string{} : std::string{input.data, input.length};
        return output.find('\0') == std::string::npos;
    }

    namespace {
        /** @brief Copies one bounded ABI node while charging its text to the complete snapshot budget. */
        Result<EditorUiNode> CopyNode(const HoroEditorActivityNode &input, std::size_t &textBytes) {
            const auto invalid = [] {
                return Result<EditorUiNode>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
            };
            EditorUiNodeBase base;
            std::string text;
            std::string action;
            if (input.structSize < sizeof(HoroEditorActivityNode) || input.kind > HORO_EDITOR_ACTIVITY_GROUP ||
                (input.flags & ~(HORO_EDITOR_ACTIVITY_NODE_DISABLED | HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL)) != 0 ||
                !CopyActivityText(input.id, base.id.value) || !CopyActivityText(input.parent, base.parent.value) ||
                !CopyActivityText(input.labelKey, base.label.value, 128) || !CopyActivityText(input.text, text, 4096) ||
                !CopyActivityText(input.actionId, action))
                return invalid();
            textBytes += base.id.value.size() + base.parent.value.size() + base.label.value.size() + text.size() + action.size();
            if (textBytes > 64U * 1024U)
                return invalid();
            base.enabled = (input.flags & HORO_EDITOR_ACTIVITY_NODE_DISABLED) == 0;
            EditorUiText content{(input.flags & HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL) != 0 ? EditorUiTextKind::TechnicalText
                                                                                          : EditorUiTextKind::LocalizationKey,
                                 std::move(text)};
            if (input.kind == HORO_EDITOR_ACTIVITY_TEXT || input.kind == HORO_EDITOR_ACTIVITY_LABEL)
                base.focusPolicy = EditorUiFocusPolicy::Never;
            switch (input.kind) {
                case HORO_EDITOR_ACTIVITY_TEXT:
                    if (!action.empty())
                        return invalid();
                    return Result<EditorUiNode>::Success({EditorUiTextNode{std::move(base), std::move(content)}});
                case HORO_EDITOR_ACTIVITY_LABEL:
                    if (!action.empty())
                        return invalid();
                    return Result<EditorUiNode>::Success({EditorUiLabelNode{std::move(base), std::move(content)}});
                case HORO_EDITOR_ACTIVITY_ACTION:
                    if (!content.value.empty() || action.empty())
                        return invalid();
                    return Result<EditorUiNode>::Success({EditorUiActionNode{.base = std::move(base), .action = {std::move(action)}}});
                default:
                    if (!action.empty() || !content.value.empty())
                        return invalid();
                    return Result<EditorUiNode>::Success(
                        {EditorUiContainerNode{.base = std::move(base),
                                               .layout = input.kind == HORO_EDITOR_ACTIVITY_GROUP ? EditorUiLayoutKind::Group
                                                                                                  : EditorUiLayoutKind::Stack}});
            }
        }
    }  // namespace

    /** @copydoc CopyActivityForm */
    Result<EditorUiForm> CopyActivityForm(const HoroEditorActivitySnapshot &snapshot, const std::string_view drawerId,
                                          const std::string_view titleKey) {
        const auto invalid = [] {
            return Result<EditorUiForm>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        };
        if (snapshot.structSize < sizeof(HoroEditorActivitySnapshot) || snapshot.schemaVersion != HORO_EDITOR_ACTIVITY_SCHEMA_VERSION ||
            snapshot.revision == 0 || (snapshot.presentationFlags & ~(HORO_EDITOR_ACTIVITY_HIDDEN | HORO_EDITOR_ACTIVITY_DISABLED)) != 0 ||
            snapshot.nodeCount > 256 || (snapshot.nodeCount != 0 && snapshot.nodes == nullptr))
            return invalid();
        EditorUiForm form{.id = {std::string{drawerId}}, .title = {EditorUiTextKind::LocalizationKey, std::string{titleKey}}};
        form.nodes.reserve(snapshot.nodeCount);
        std::size_t textBytes{};
        for (std::uint32_t index = 0; index < snapshot.nodeCount; ++index) {
            const auto &input = snapshot.nodes[index];
            auto node = CopyNode(input, textBytes);
            if (node.HasError())
                return Result<EditorUiForm>::Failure(node.ErrorValue());
            form.nodes.push_back(std::move(node).Value());
        }
        if (const auto valid = ValidateEditorUiForm(form); valid.HasError())
            return Result<EditorUiForm>::Failure(valid.ErrorValue());
        return Result<EditorUiForm>::Success(std::move(form));
    }
}  // namespace Horo::Extensions::Detail
