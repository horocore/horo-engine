#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] Result<void> ThemeInvalid(const std::string_view reason) {
            return Result<void>::Failure(MakeError(ExtensionErrors::EditorUiThemeInvalid, std::string{reason}));
        }

        [[nodiscard]] bool IsInteractive(const EditorUiNodeKind kind) noexcept {
            using enum EditorUiNodeKind;
            switch (kind) {
                case TextField:
                case Number:
                case Boolean:
                case Choice:
                case Path:
                case Color:
                case Vector:
                case Action:
                    return true;
                case Label:
                case Text:
                case Validation:
                case Group:
                case Stack:
                case Row:
                case Grid:
                case Help:
                    return false;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool IsContainer(const EditorUiNodeKind kind) noexcept {
            using enum EditorUiNodeKind;
            return kind == Group || kind == Stack || kind == Row || kind == Grid;
        }

        [[nodiscard]] const EditorUiNodeBase &BaseOf(const EditorUiNode &node) noexcept {
            return EditorUiNodeBaseOf(node);
        }

        [[nodiscard]] std::size_t FindNodeIndex(const std::vector<EditorUiNode> &nodes, const EditorUiId &id) noexcept {
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (BaseOf(nodes[index]).id == id)
                    return index;
            }
            return nodes.size();
        }

        [[nodiscard]] bool IsValidMetric(const float value) noexcept {
            return std::isfinite(value) && value > 0.0F;
        }

        [[nodiscard]] Result<void> ValidateTheme(const EditorUiThemeFrame &theme) {
            if (const std::uint32_t knownTokenMask =
                    EditorUiThemeTokenBit(EditorUiThemeToken::Critical) | (EditorUiThemeTokenBit(EditorUiThemeToken::Critical) - 1U);
                theme.schemaVersion != EditorUiFormSchemaVersion || theme.revision == 0 || !std::isfinite(theme.uiScale) ||
                theme.uiScale <= 0.0F || theme.uiScale > 8.0F || (theme.supportedTokenMask & ~knownTokenMask) != 0U ||
                !IsValidMetric(theme.metrics.smallControlHeight) || !IsValidMetric(theme.metrics.mediumControlHeight) ||
                !IsValidMetric(theme.metrics.largeControlHeight) || !IsValidMetric(theme.metrics.textLineHeight) ||
                !IsValidMetric(theme.metrics.rowGap) || !IsValidMetric(theme.metrics.defaultWidth))
                return ThemeInvalid("Editor UI theme frame has an unsupported schema, scale, token set, or metric.");
            if (theme.metrics.smallControlHeight > theme.metrics.mediumControlHeight ||
                theme.metrics.mediumControlHeight > theme.metrics.largeControlHeight)
                return ThemeInvalid("Editor UI theme control heights must be ordered from small to large.");
            return Result<void>::Success();
        }

        [[nodiscard]] std::array<EditorUiThemeToken, 2> FallbackTokens(const EditorUiThemeToken requested) noexcept {
            using enum EditorUiThemeToken;
            switch (requested) {
                case SurfaceSubtle:
                    return {Surface, None};
                case TextDisabled:
                    return {TextSecondary, TextPrimary};
                case Border:
                case Focus:
                    return {Accent, TextSecondary};
                case Positive:
                case Warning:
                case Critical:
                case Accent:
                    return {Accent, TextPrimary};
                case TextSecondary:
                    return {TextPrimary, None};
                case Surface:
                case TextPrimary:
                case None:
                    return {None, None};
                default:
                    return {None, None};
            }
        }

        [[nodiscard]] EditorUiThemeToken ResolveToken(const EditorUiThemeToken requested, const EditorUiThemeFrame &frame) noexcept {
            if (requested == EditorUiThemeToken::None)
                return EditorUiThemeToken::None;
            if ((frame.supportedTokenMask & EditorUiThemeTokenBit(requested)) != 0U)
                return requested;

            const auto fallbacks = FallbackTokens(requested);
            for (const auto fallback : fallbacks) {
                if (fallback != EditorUiThemeToken::None && (frame.supportedTokenMask & EditorUiThemeTokenBit(fallback)) != 0U)
                    return fallback;
            }
            return EditorUiThemeToken::None;
        }

        [[nodiscard]] EditorUiThemeToken ToneToken(const EditorUiSemanticTone tone) noexcept {
            using enum EditorUiSemanticTone;
            switch (tone) {
                case Neutral:
                case Secondary:
                    return EditorUiThemeToken::Border;
                case Primary:
                    return EditorUiThemeToken::Accent;
                case Positive:
                    return EditorUiThemeToken::Positive;
                case Warning:
                    return EditorUiThemeToken::Warning;
                case Critical:
                    return EditorUiThemeToken::Critical;
                default:
                    return EditorUiThemeToken::None;
            }
        }

        [[nodiscard]] EditorUiThemeToken ValidationToken(const EditorUiValidationSeverity severity) noexcept {
            using enum EditorUiValidationSeverity;
            switch (severity) {
                case Info:
                    return EditorUiThemeToken::Accent;
                case Warning:
                    return EditorUiThemeToken::Warning;
                case Error:
                    return EditorUiThemeToken::Critical;
                default:
                    return EditorUiThemeToken::None;
            }
        }

        [[nodiscard]] float ControlHeight(const EditorUiComponentSize size, const EditorUiThemeMetrics &metrics) noexcept {
            using enum EditorUiComponentSize;
            switch (size) {
                case Small:
                    return metrics.smallControlHeight;
                case Medium:
                    return metrics.mediumControlHeight;
                case Large:
                    return metrics.largeControlHeight;
                default:
                    return metrics.mediumControlHeight;
            }
        }

        [[nodiscard]] float NodeHeight(const EditorUiNodeKind kind, const EditorUiComponentSize size,
                                       const EditorUiThemeMetrics &metrics) noexcept {
            using enum EditorUiNodeKind;
            switch (kind) {
                case TextField:
                case Number:
                case Boolean:
                case Choice:
                case Path:
                case Color:
                case Vector:
                case Action:
                    return ControlHeight(size, metrics);
                case Label:
                case Text:
                case Validation:
                case Group:
                case Stack:
                case Row:
                case Grid:
                case Help:
                    return metrics.textLineHeight;
                default:
                    return metrics.textLineHeight;
            }
        }

        struct ResolvedStyle final {
            EditorUiThemeToken foreground{EditorUiThemeToken::TextPrimary};
            EditorUiThemeToken background{EditorUiThemeToken::None};
            EditorUiThemeToken border{EditorUiThemeToken::None};
        };

        [[nodiscard]] EditorUiThemeToken ActionBorderToken(const EditorUiActionKind actionKind) noexcept {
            using enum EditorUiActionKind;
            switch (actionKind) {
                case Primary:
                    return EditorUiThemeToken::Accent;
                case Destructive:
                    return EditorUiThemeToken::Critical;
                case Secondary:
                    return EditorUiThemeToken::Border;
                default:
                    return EditorUiThemeToken::Border;
            }
        }

        [[nodiscard]] ResolvedStyle StyleForNode(const EditorUiNode &node, const EditorUiThemeFrame &theme) noexcept {
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const EditorUiNodeBase &base = BaseOf(node);
            ResolvedStyle style;
            if (kind == EditorUiNodeKind::Help)
                style.foreground = EditorUiThemeToken::TextSecondary;
            else if (kind == EditorUiNodeKind::Validation) {
                const auto severity = std::get<EditorUiValidationNode>(node.payload).severity;
                if (severity == EditorUiValidationSeverity::Info)
                    style.foreground = EditorUiThemeToken::TextSecondary;
                else
                    style.foreground = ValidationToken(severity);
            } else if (IsInteractive(kind)) {
                style.background = EditorUiThemeToken::Surface;
                if (kind == EditorUiNodeKind::Action) {
                    style.border = ActionBorderToken(std::get<EditorUiActionNode>(node.payload).actionKind);
                } else {
                    style.border = EditorUiThemeToken::Border;
                }
            } else if (IsContainer(kind)) {
                style.background = kind == EditorUiNodeKind::Group ? EditorUiThemeToken::SurfaceSubtle : EditorUiThemeToken::None;
            }
            if (base.tone != EditorUiSemanticTone::Neutral && kind != EditorUiNodeKind::Validation)
                style.border = ToneToken(base.tone);
            if (!base.enabled) {
                style.foreground = EditorUiThemeToken::TextDisabled;
                style.background = EditorUiThemeToken::SurfaceSubtle;
            }
            style.foreground = ResolveToken(style.foreground, theme);
            style.background = ResolveToken(style.background, theme);
            style.border = ResolveToken(style.border, theme);
            return style;
        }

        [[nodiscard]] std::size_t SiblingIndex(const std::vector<EditorUiNode> &nodes, const std::size_t nodeIndex) noexcept {
            const EditorUiId parent = BaseOf(nodes[nodeIndex]).parent;
            std::size_t siblingIndex = 0;
            for (std::size_t index = 0; index < nodeIndex; ++index) {
                if (BaseOf(nodes[index]).parent == parent)
                    ++siblingIndex;
            }
            return siblingIndex;
        }

        [[nodiscard]] std::size_t SiblingCount(const std::vector<EditorUiNode> &nodes, const EditorUiId &parent) noexcept {
            return static_cast<std::size_t>(std::ranges::count_if(nodes, [&](const EditorUiNode &node) {
                return BaseOf(node).parent == parent;
            }));
        }

        [[nodiscard]] std::pair<float, float> ResponsiveWidthAndOffset(const std::vector<EditorUiNode> &nodes, const std::size_t nodeIndex,
                                                                       const float width) noexcept {
            const EditorUiId parent = BaseOf(nodes[nodeIndex]).parent;
            if (parent.value.empty())
                return {width, 0.0F};
            const std::size_t parentIndex = FindNodeIndex(nodes, parent);
            if (parentIndex >= nodes.size())
                return {width, 0.0F};
            const EditorUiNodeKind parentKind = EditorUiNodeKindOf(nodes[parentIndex]);
            const std::size_t siblings = std::max<std::size_t>(SiblingCount(nodes, parent), 1U);
            std::size_t columns = 1;
            if (parentKind == EditorUiNodeKind::Row)
                columns = siblings;
            else if (parentKind == EditorUiNodeKind::Grid)
                columns = std::max<std::size_t>(std::get<EditorUiContainerNode>(nodes[parentIndex].payload).columns, 1U);
            if (columns == 1)
                return {width, 0.0F};
            const float cellWidth = width / static_cast<float>(columns);
            const std::size_t column = SiblingIndex(nodes, nodeIndex) % columns;
            return {cellWidth, cellWidth * static_cast<float>(column)};
        }

        [[nodiscard]] Result<EditorUiRenderSnapshot> CreateRenderSnapshot(const EditorUiForm &form, const EditorUiThemeFrame &theme,
                                                                          const float logicalWidth) {
            EditorUiRenderSnapshot snapshot{.schemaVersion = EditorUiFormSchemaVersion,
                                            .form = form.id,
                                            .themeRevision = theme.revision,
                                            .uiScale = theme.uiScale,
                                            .availableWidth = logicalWidth * theme.uiScale};
            try {
                snapshot.nodes.reserve(form.nodes.size());
            } catch (const std::bad_alloc &) {
                return Result<EditorUiRenderSnapshot>::Failure(
                    MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Editor UI render snapshot storage could not be reserved."));
            }
            return Result<EditorUiRenderSnapshot>::Success(std::move(snapshot));
        }

        void AppendRenderNode(EditorUiRenderSnapshot &snapshot, const std::vector<EditorUiNode> &nodes, const std::size_t index,
                              const EditorUiThemeFrame &theme, const float logicalWidth, float &cursorY, std::uint32_t &focusOrder) {
            const EditorUiNode &node = nodes[index];
            const EditorUiNodeBase &base = BaseOf(node);
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const auto [logicalNodeWidth, logicalNodeX] = ResponsiveWidthAndOffset(nodes, index, logicalWidth);
            const float logicalHeight = NodeHeight(kind, base.size, theme.metrics);
            const ResolvedStyle style = StyleForNode(node, theme);
            EditorUiRenderNode renderNode{.id = base.id,
                                          .parent = base.parent,
                                          .kind = kind,
                                          .size = base.size,
                                          .tone = base.tone,
                                          .foregroundToken = style.foreground,
                                          .backgroundToken = style.background,
                                          .borderToken = style.border,
                                          .enabled = base.enabled,
                                          .readOnly = base.readOnly,
                                          .focusable =
                                              base.enabled && base.focusPolicy == EditorUiFocusPolicy::Automatic && IsInteractive(kind),
                                          .focusOrder = 0,
                                          .x = logicalNodeX * theme.uiScale,
                                          .y = cursorY,
                                          .width = logicalNodeWidth * theme.uiScale,
                                          .height = logicalHeight * theme.uiScale};
            if (renderNode.focusable)
                renderNode.focusOrder = ++focusOrder;
            snapshot.nodes.push_back(std::move(renderNode));
            cursorY += (logicalHeight + theme.metrics.rowGap) * theme.uiScale;
        }
    }  // namespace

    /** @copydoc ResolveEditorUiThemeToken */
    EditorUiThemeToken ResolveEditorUiThemeToken(const EditorUiThemeToken requested, const EditorUiThemeFrame &frame) noexcept {
        return ResolveToken(requested, frame);
    }

    /** @copydoc BuildEditorUiRenderSnapshot */
    Result<EditorUiRenderSnapshot> BuildEditorUiRenderSnapshot(const EditorUiForm &form, const EditorUiThemeFrame &theme,
                                                               const float availableWidth) {
        if (Result<void> validation = ValidateEditorUiForm(form); validation.HasError())
            return Result<EditorUiRenderSnapshot>::Failure(std::move(validation).ErrorValue());
        if (Result<void> validation = ValidateTheme(theme); validation.HasError())
            return Result<EditorUiRenderSnapshot>::Failure(std::move(validation).ErrorValue());
        const float logicalWidth = availableWidth == 0.0F ? theme.metrics.defaultWidth : availableWidth;
        if (!std::isfinite(logicalWidth) || logicalWidth <= 0.0F || logicalWidth > 32'768.0F)
            return Result<EditorUiRenderSnapshot>::Failure(
                MakeError(ExtensionErrors::EditorUiThemeInvalid, "Editor UI available width is outside the finite adapter bound."));

        auto snapshotResult = CreateRenderSnapshot(form, theme, logicalWidth);
        if (snapshotResult.HasError())
            return Result<EditorUiRenderSnapshot>::Failure(std::move(snapshotResult).ErrorValue());
        auto snapshot = std::move(snapshotResult).Value();
        float cursorY = 0.0F;
        std::uint32_t focusOrder = 0;
        for (std::size_t index = 0; index < form.nodes.size(); ++index) {
            AppendRenderNode(snapshot, form.nodes, index, theme, logicalWidth, cursorY, focusOrder);
        }
        return Result<EditorUiRenderSnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::Extensions
