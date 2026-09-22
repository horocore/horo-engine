#include "Horo/Extensions/EditorUiForm.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <optional>
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
            using enum EditorUiThemeToken;
            if (requested == None)
                return None;
            if ((frame.supportedTokenMask & EditorUiThemeTokenBit(requested)) != 0U)
                return requested;

            const auto fallbacks = FallbackTokens(requested);
            for (const auto fallback : fallbacks) {
                if (fallback != None && (frame.supportedTokenMask & EditorUiThemeTokenBit(fallback)) != 0U)
                    return fallback;
            }
            return None;
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
            using enum EditorUiThemeToken;
            switch (actionKind) {
                case Primary:
                    return Accent;
                case Destructive:
                    return Critical;
                case Secondary:
                    return Border;
                default:
                    return Border;
            }
        }

        [[nodiscard]] ResolvedStyle StyleForNode(const EditorUiNode &node, const EditorUiThemeFrame &theme) noexcept {
            using enum EditorUiThemeToken;
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const EditorUiNodeBase &base = BaseOf(node);
            ResolvedStyle style;
            if (kind == EditorUiNodeKind::Help)
                style.foreground = TextSecondary;
            else if (kind == EditorUiNodeKind::Validation) {
                const auto severity = std::get<EditorUiValidationNode>(node.payload).severity;
                if (severity == EditorUiValidationSeverity::Info)
                    style.foreground = TextSecondary;
                else
                    style.foreground = ValidationToken(severity);
            } else if (IsInteractive(kind)) {
                style.background = Surface;
                if (kind == EditorUiNodeKind::Action) {
                    style.border = ActionBorderToken(std::get<EditorUiActionNode>(node.payload).actionKind);
                } else {
                    style.border = Border;
                }
            } else if (IsContainer(kind)) {
                style.background = kind == EditorUiNodeKind::Group ? SurfaceSubtle : None;
            }
            if (base.tone != EditorUiSemanticTone::Neutral && kind != EditorUiNodeKind::Validation)
                style.border = ToneToken(base.tone);
            if (!base.enabled) {
                style.foreground = TextDisabled;
                style.background = SurfaceSubtle;
            }
            style.foreground = ResolveToken(style.foreground, theme);
            style.background = ResolveToken(style.background, theme);
            style.border = ResolveToken(style.border, theme);
            return style;
        }

        struct LayoutBounds final {
            float x{};
            float y{};
            float width{};
            float height{};
        };

        class LayoutPlan final {
        public:
            LayoutPlan(const std::vector<EditorUiNode> &nodes, const EditorUiThemeMetrics &metrics, const float width)
                : nodes_(nodes), metrics_(metrics), width_(width), heights_(nodes.size()), bounds_(nodes.size()) {}

            void Build() {
                for (std::size_t index = 0; index < nodes_.size(); ++index) {
                    if (BaseOf(nodes_[index]).parent.value.empty())
                        heights_[index] = MeasureNode(index, width_);
                }

                float cursorY = 0.0F;
                for (std::size_t index = 0; index < nodes_.size(); ++index) {
                    if (!BaseOf(nodes_[index]).parent.value.empty())
                        continue;
                    PlaceNode(index, 0.0F, cursorY, width_);
                    cursorY += heights_[index] + metrics_.rowGap;
                }
            }

            [[nodiscard]] const LayoutBounds &Bounds(const std::size_t index) const noexcept {
                return bounds_[index];
            }

        private:
            template <typename Function> void ForEachChild(const EditorUiId &parent, Function &&function) const {
                for (std::size_t index = 0; index < nodes_.size(); ++index) {
                    if (BaseOf(nodes_[index]).parent == parent)
                        function(index);
                }
            }

            [[nodiscard]] std::size_t ChildCount(const EditorUiId &parent) const noexcept {
                return static_cast<std::size_t>(std::ranges::count_if(nodes_, [&parent](const EditorUiNode &node) {
                    return BaseOf(node).parent == parent;
                }));
            }

            [[nodiscard]] std::size_t GridColumns(const EditorUiContainerNode &container) const noexcept {
                return std::max<std::size_t>(container.columns, 1U);
            }

            [[nodiscard]] float MeasureVertical(const EditorUiId &parent, const float width) {
                float height = 0.0F;
                std::size_t childCount = 0;
                ForEachChild(parent, [this, &height, &childCount, width](const std::size_t childIndex) {
                    if (childCount != 0)
                        height += metrics_.rowGap;
                    height += MeasureNode(childIndex, width);
                    ++childCount;
                });
                return height;
            }

            [[nodiscard]] float MeasureRow(const EditorUiId &parent, const float width) {
                const std::size_t childCount = ChildCount(parent);
                if (childCount == 0)
                    return metrics_.textLineHeight;
                const float childWidth = width / static_cast<float>(childCount);
                float height = 0.0F;
                ForEachChild(parent, [this, &height, childWidth](const std::size_t childIndex) {
                    height = std::max(height, MeasureNode(childIndex, childWidth));
                });
                return height;
            }

            [[nodiscard]] float MeasureGrid(const EditorUiContainerNode &container, const float width) {
                const std::size_t columns = GridColumns(container);
                const float childWidth = width / static_cast<float>(columns);
                float height = 0.0F;
                float rowHeight = 0.0F;
                std::size_t column = 0;
                std::size_t rowCount = 0;
                ForEachChild(container.base.id,
                             [this, &height, &rowHeight, &column, &rowCount, childWidth, columns](const std::size_t childIndex) {
                    rowHeight = std::max(rowHeight, MeasureNode(childIndex, childWidth));
                    ++column;
                    if (column == columns) {
                        if (rowCount != 0)
                            height += metrics_.rowGap;
                        height += rowHeight;
                        ++rowCount;
                        rowHeight = 0.0F;
                        column = 0;
                    }
                });
                if (column != 0) {
                    if (rowCount != 0)
                        height += metrics_.rowGap;
                    height += rowHeight;
                }
                return height == 0.0F ? metrics_.textLineHeight : height;
            }

            [[nodiscard]] float MeasureContainer(const EditorUiContainerNode &container, const float width) {
                using enum EditorUiLayoutKind;
                if (ChildCount(container.base.id) == 0)
                    return metrics_.textLineHeight;
                switch (container.layout) {
                    case Group:
                    case Stack:
                        return MeasureVertical(container.base.id, width);
                    case Row:
                        return MeasureRow(container.base.id, width);
                    case Grid:
                        return MeasureGrid(container, width);
                    default:
                        return metrics_.textLineHeight;
                }
            }

            [[nodiscard]] float MeasureNode(const std::size_t index, const float width) {
                const EditorUiNode &node = nodes_[index];
                const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
                const EditorUiNodeBase &base = BaseOf(node);
                const float baseHeight = NodeHeight(kind, base.size, metrics_);
                float height = baseHeight;
                if (IsContainer(kind))
                    height = MeasureContainer(std::get<EditorUiContainerNode>(node.payload), width);
                else if (ChildCount(base.id) != 0)
                    height += metrics_.rowGap + MeasureVertical(base.id, width);
                heights_[index] = height;
                return height;
            }

            [[nodiscard]] float GridRowOffset(const EditorUiId &parent, const std::size_t targetRow,
                                              const std::size_t columns) const noexcept {
                float offset = 0.0F;
                float rowHeight = 0.0F;
                std::size_t ordinal = 0;
                for (std::size_t index = 0; index < nodes_.size(); ++index) {
                    if (BaseOf(nodes_[index]).parent != parent)
                        continue;
                    if (const std::size_t row = ordinal / columns; row >= targetRow)
                        break;
                    rowHeight = std::max(rowHeight, heights_[index]);
                    ++ordinal;
                    if (ordinal % columns == 0) {
                        offset += rowHeight + metrics_.rowGap;
                        rowHeight = 0.0F;
                    }
                }
                return offset;
            }

            void PlaceVertical(const EditorUiContainerNode &container, const float x, const float y, const float width) {
                float cursorY = y;
                ForEachChild(container.base.id, [this, &cursorY, x, width](const std::size_t childIndex) {
                    PlaceNode(childIndex, x, cursorY, width);
                    cursorY += heights_[childIndex] + metrics_.rowGap;
                });
            }

            void PlaceRow(const EditorUiContainerNode &container, const float x, const float y, const float width) {
                const std::size_t childCount = ChildCount(container.base.id);
                if (childCount == 0)
                    return;
                const float childWidth = width / static_cast<float>(childCount);
                std::size_t column = 0;
                ForEachChild(container.base.id, [this, &column, childWidth, x, y](const std::size_t childIndex) {
                    PlaceNode(childIndex, x + childWidth * static_cast<float>(column), y, childWidth);
                    ++column;
                });
            }

            void PlaceGrid(const EditorUiContainerNode &container, const float x, const float y, const float width) {
                const std::size_t columns = GridColumns(container);
                const float childWidth = width / static_cast<float>(columns);
                std::size_t ordinal = 0;
                ForEachChild(container.base.id, [this, &ordinal, columns, childWidth, x, y, &container](const std::size_t childIndex) {
                    const std::size_t row = ordinal / columns;
                    const std::size_t column = ordinal % columns;
                    const float rowY = y + GridRowOffset(container.base.id, row, columns);
                    PlaceNode(childIndex, x + childWidth * static_cast<float>(column), rowY, childWidth);
                    ++ordinal;
                });
            }

            void PlaceNode(const std::size_t index, const float x, const float y, const float width) {
                bounds_[index] = {.x = x, .y = y, .width = width, .height = heights_[index]};
                const EditorUiNode &node = nodes_[index];
                const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
                const EditorUiNodeBase &base = BaseOf(node);
                if (!IsContainer(kind)) {
                    float cursorY = y + NodeHeight(kind, base.size, metrics_);
                    if (ChildCount(base.id) != 0) {
                        cursorY += metrics_.rowGap;
                        ForEachChild(base.id, [this, &cursorY, x, width](const std::size_t childIndex) {
                            PlaceNode(childIndex, x, cursorY, width);
                            cursorY += heights_[childIndex] + metrics_.rowGap;
                        });
                    }
                    return;
                }
                const auto &container = std::get<EditorUiContainerNode>(node.payload);
                using enum EditorUiLayoutKind;
                switch (container.layout) {
                    case Group:
                    case Stack:
                        PlaceVertical(container, x, y, width);
                        return;
                    case Row:
                        PlaceRow(container, x, y, width);
                        return;
                    case Grid:
                        PlaceGrid(container, x, y, width);
                        return;
                    default:
                        return;
                }
            }

            const std::vector<EditorUiNode> &nodes_;
            const EditorUiThemeMetrics &metrics_;
            float width_;
            std::vector<float> heights_;
            std::vector<LayoutBounds> bounds_;
        };

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
                              const EditorUiThemeFrame &theme, const LayoutPlan &layout, std::uint32_t &focusOrder) {
            const EditorUiNode &node = nodes[index];
            const EditorUiNodeBase &base = BaseOf(node);
            const EditorUiNodeKind kind = EditorUiNodeKindOf(node);
            const LayoutBounds &bounds = layout.Bounds(index);
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
                                          .x = bounds.x * theme.uiScale,
                                          .y = bounds.y * theme.uiScale,
                                          .width = bounds.width * theme.uiScale,
                                          .height = bounds.height * theme.uiScale};
            if (renderNode.focusable)
                renderNode.focusOrder = ++focusOrder;
            snapshot.nodes.push_back(std::move(renderNode));
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
        std::optional<LayoutPlan> layout;
        try {
            layout.emplace(form.nodes, theme.metrics, logicalWidth);
            layout->Build();
        } catch (const std::bad_alloc &) {
            return Result<EditorUiRenderSnapshot>::Failure(
                MakeError(ExtensionErrors::EditorUiFormCapacityExceeded, "Editor UI layout storage could not be reserved."));
        }
        std::uint32_t focusOrder = 0;
        for (std::size_t index = 0; index < form.nodes.size(); ++index) {
            AppendRenderNode(snapshot, form.nodes, index, theme, *layout, focusOrder);
        }
        return Result<EditorUiRenderSnapshot>::Success(std::move(snapshot));
    }
}  // namespace Horo::Extensions
