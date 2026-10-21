#include "EditorActivityAbiConversion.h"

#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::Detail {
    static_assert(HORO_EDITOR_UI_FONT_COUNT == static_cast<std::uint32_t>(EditorThemeFontRole::Count));
    static_assert(HORO_EDITOR_UI_ICON_COUNT == static_cast<std::uint32_t>(EditorThemeIconRole::Count));
    static_assert(HORO_EDITOR_UI_COLOR_COUNT == static_cast<std::uint32_t>(EditorThemeColorRole::Count));
    static_assert(HORO_EDITOR_UI_MOTION_COUNT == static_cast<std::uint32_t>(EditorThemeMotionRole::Count));

    /** @copydoc CopyCustomPresentation */
    Result<std::optional<EditorUiCustomPresentation>> CopyCustomPresentation(const HoroEditorUiCustomPresentation *input) {
        if (!input)
            return Result<std::optional<EditorUiCustomPresentation>>::Success(std::nullopt);
        if (!HoroEditorUiCustomPresentationShape(input))
            return Result<std::optional<EditorUiCustomPresentation>>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        return Result<std::optional<EditorUiCustomPresentation>>::Success(
            EditorUiCustomPresentation{input->schemaVersion, static_cast<EditorUiCustomWidget>(input->widget),
                                       static_cast<EditorThemeFontRole>(input->fontRole), static_cast<EditorThemeIconRole>(input->iconRole),
                                       static_cast<EditorThemeColorRole>(input->accentRole),
                                       static_cast<EditorThemeMotionRole>(input->motionRole),
                                       static_cast<std::uint8_t>(input->controlRows)});
    }

    /** @copydoc CopyActivityText */
    bool CopyActivityText(const HoroExtensionStringView input, std::string &output, const std::size_t maximum) {
        if (input.length > maximum || (input.length != 0 && input.data == nullptr))
            return false;
        output = input.length == 0 ? std::string{} : std::string{input.data, input.length};
        return output.find('\0') == std::string::npos;
    }

    namespace {
        static_assert(MaximumEditorUiPlotSamples == HORO_EDITOR_ACTIVITY_MAXIMUM_PLOT_SAMPLES);

        /** @brief Copies already bounded semantic samples; no renderer resource or borrowed storage escapes. */
        Result<EditorUiNode> CopyPlotNode(EditorUiNodeBase base, std::string action, const HoroEditorActivityNodeV2 *input) {
            if (action.empty() || !HoroEditorActivityPlotNodeShape(input))
                return Result<EditorUiNode>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
            EditorUiPlotNode plot{.base = std::move(base), .action = {std::move(action)}};
            plot.samples.reserve(input->sampleCount);
            for (std::uint32_t i = 0; i < input->sampleCount; ++i)
                plot.samples.push_back({input->samples[i].x, input->samples[i].y});
            return Result<EditorUiNode>::Success({std::move(plot)});
        }

        /** @brief Resolves only registered standard layout semantics; no absolute screen placement crosses the ABI. */
        EditorUiLayoutKind LayoutKind(const std::uint32_t kind) noexcept {
            switch (kind) {
                case HORO_EDITOR_ACTIVITY_GROUP:
                    return EditorUiLayoutKind::Group;
                case HORO_EDITOR_ACTIVITY_ROW:
                    return EditorUiLayoutKind::Row;
                case HORO_EDITOR_ACTIVITY_GRID:
                    return EditorUiLayoutKind::Grid;
                default:
                    return EditorUiLayoutKind::Stack;
            }
        }

        /** @brief Builds the closed standard vocabulary after common text and permission identities have been copied. */
        Result<EditorUiNode> CopyStandardNode(const std::uint32_t kind, EditorUiNodeBase base, EditorUiText content, std::string action,
                                              const std::uint8_t columns) {
            const auto invalid = [] {
                return Result<EditorUiNode>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
            };
            switch (kind) {
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
                        {EditorUiContainerNode{.base = std::move(base), .layout = LayoutKind(kind), .columns = columns}});
            }
        }

        /** @brief Deep-copies the optional schema two metadata under the same aggregate text budget. */
        bool CopyPresentation(const HoroEditorActivityNodeV2 *advanced, EditorUiNodeBase &base, std::size_t &textBytes) {
            if (!advanced)
                return true;
            auto style = CopyCustomPresentation(advanced->presentation);
            if (style.HasError() || !CopyActivityText(advanced->descriptionKey, base.description.value, 128) ||
                !CopyActivityText(advanced->accessibleLabelKey, base.accessibleLabel.value, 128) || advanced->columns > 16)
                return false;
            base.custom = std::move(style).Value();
            textBytes += base.description.value.size() + base.accessibleLabel.value.size();
            return textBytes <= 64U * 1024U;
        }

        /** @brief Copies one bounded ABI node while charging its text to the complete snapshot budget. */
        Result<EditorUiNode> CopyNode(const HoroEditorActivityNode &input, std::size_t &textBytes,
                                      const HoroEditorActivityNodeV2 *advanced) {
            const auto invalid = [] {
                return Result<EditorUiNode>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
            };
            EditorUiNodeBase base;
            std::string text;
            std::string action;
            if (input.structSize < sizeof(HoroEditorActivityNode) || input.kind > HORO_EDITOR_ACTIVITY_GRID ||
                (!advanced && input.kind > HORO_EDITOR_ACTIVITY_GROUP) ||
                (input.flags & ~(HORO_EDITOR_ACTIVITY_NODE_DISABLED | HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL)) != 0 ||
                !CopyActivityText(input.id, base.id.value) || !CopyActivityText(input.parent, base.parent.value) ||
                !CopyActivityText(input.labelKey, base.label.value, 128) || !CopyActivityText(input.text, text, 4096) ||
                !CopyActivityText(input.actionId, action))
                return invalid();
            textBytes += base.id.value.size() + base.parent.value.size() + base.label.value.size() + text.size() + action.size();
            if (textBytes > 64U * 1024U)
                return invalid();
            if (!CopyPresentation(advanced, base, textBytes))
                return invalid();
            base.enabled = (input.flags & HORO_EDITOR_ACTIVITY_NODE_DISABLED) == 0;
            EditorUiText content{(input.flags & HORO_EDITOR_ACTIVITY_TEXT_TECHNICAL) != 0 ? EditorUiTextKind::TechnicalText
                                                                                          : EditorUiTextKind::LocalizationKey,
                                 std::move(text)};
            if (input.kind == HORO_EDITOR_ACTIVITY_TEXT || input.kind == HORO_EDITOR_ACTIVITY_LABEL ||
                input.kind == HORO_EDITOR_ACTIVITY_STACK || input.kind == HORO_EDITOR_ACTIVITY_GROUP ||
                input.kind == HORO_EDITOR_ACTIVITY_ROW || input.kind == HORO_EDITOR_ACTIVITY_GRID)
                base.focusPolicy = EditorUiFocusPolicy::Never;
            if (input.kind == HORO_EDITOR_ACTIVITY_PLOT) {
                if (!content.value.empty())
                    return invalid();
                return CopyPlotNode(std::move(base), std::move(action), advanced);
            }
            if (advanced && (advanced->samples || advanced->sampleCount != 0))
                return invalid();
            if ((input.kind == HORO_EDITOR_ACTIVITY_GRID && (!advanced || advanced->columns == 0)) ||
                (input.kind != HORO_EDITOR_ACTIVITY_GRID && advanced && advanced->columns > 1))
                return invalid();
            return CopyStandardNode(input.kind, std::move(base), std::move(content), std::move(action),
                                    input.kind == HORO_EDITOR_ACTIVITY_GRID ? static_cast<std::uint8_t>(advanced->columns) : 1);
        }
    }  // namespace

    /** @copydoc CopyActivityForm */
    Result<EditorUiForm> CopyActivityForm(const HoroEditorActivitySnapshot &snapshot, const std::string_view drawerId,
                                          const std::string_view titleKey) {
        const auto invalid = [] {
            return Result<EditorUiForm>::Failure(MakeError(ExtensionErrors::EditorUiFormInvalid));
        };
        if (snapshot.structSize < sizeof(HoroEditorActivitySnapshot) ||
            (snapshot.schemaVersion != HORO_EDITOR_ACTIVITY_SCHEMA_VERSION &&
             snapshot.schemaVersion != HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION) ||
            snapshot.revision == 0 || (snapshot.presentationFlags & ~(HORO_EDITOR_ACTIVITY_HIDDEN | HORO_EDITOR_ACTIVITY_DISABLED)) != 0 ||
            snapshot.nodeCount > 256 || (snapshot.nodeCount != 0 && snapshot.nodes == nullptr))
            return invalid();
        EditorUiForm form{.id = {std::string{drawerId}}, .title = {EditorUiTextKind::LocalizationKey, std::string{titleKey}}};
        form.nodes.reserve(snapshot.nodeCount);
        std::size_t textBytes{};
        for (std::uint32_t index = 0; index < snapshot.nodeCount; ++index) {
            const auto *advanced = snapshot.schemaVersion == HORO_EDITOR_ACTIVITY_PLOT_SCHEMA_VERSION
                                       ? reinterpret_cast<const HoroEditorActivityNodeV2 *>(snapshot.nodes) + index
                                       : nullptr;
            if (advanced && !HoroEditorActivityNodeV2Shape(advanced))
                return invalid();
            const auto &input = advanced ? advanced->node : snapshot.nodes[index];
            auto node = CopyNode(input, textBytes, advanced);
            if (node.HasError())
                return Result<EditorUiForm>::Failure(node.ErrorValue());
            form.nodes.push_back(std::move(node).Value());
        }
        if (const auto valid = ValidateEditorUiForm(form); valid.HasError())
            return Result<EditorUiForm>::Failure(valid.ErrorValue());
        return Result<EditorUiForm>::Success(std::move(form));
    }
}  // namespace Horo::Extensions::Detail
