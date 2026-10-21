#include "editor/screens/workspace/panels/inspector/UiInspectorWidgets.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace Horo::Editor::UiInspectorWidgets {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Shared localized presentation labels never become authored property keys. */
        const char *Label(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key).c_str();
        }

        /** @brief Exact integer editing retains invalid text and never publishes a substitute value. */
        template <typename T> bool Integer(const EditorGuiContext &context, const char *key, T &value, UiInspectorInteger &text) {
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            // A foreign unterminated draft stays invalid; the shared control always receives bounded text.
            if (std::find(text.bytes.begin(), text.bytes.end(), '\0') == text.bytes.end()) {
                text.bytes.back() = '\0';
                text.valid = false;
            }
            if (!Ui::InputTextControl(key, text.bytes.data(), text.bytes.size(), context.theme.fonts, {.error = !text.valid}))
                return false;
            static_cast<void>(text.Parse(value));
            return true;
        }

        /** @brief Draw a canonical logical length with explicit unit; switching unit preserves magnitude. */
        bool Length(const EditorGuiContext &context, const char *key, UiLength &value, UiInspectorInteger &text) {
            ImGui::PushID(key);
            const char *units[]{Label(context, "workspace.ui_inspector.auto"), Label(context, "workspace.ui_inspector.dip"),
                                Label(context, "workspace.ui_inspector.percent")};
            int unit = static_cast<int>(value.kind);
            bool changed = Ui::DrawComboPropRow(Label(context, key), "unit", unit, units, context.theme.fonts);
            if (changed) {
                value.kind = static_cast<UiLengthKind>(unit);
                if (value.kind == UiLengthKind::Auto) {
                    value.value = 0;
                    text.Capture(value.value);
                }
            }
            if (value.kind != UiLengthKind::Auto)
                changed = Integer(context,
                                  value.kind == UiLengthKind::Percent ? "workspace.ui_inspector.normalized_value"
                                                                      : "workspace.ui_inspector.logical_value",
                                  value.value, text) ||
                          changed;
            ImGui::PopID();
            return changed;
        }

        /** @brief Visit only the six canonical sizing fields in fixed order. */
        template <typename T, typename F> void Sizes(T &value, F action) {
            action("workspace.ui_inspector.width", value.width);
            action("workspace.ui_inspector.height", value.height);
            action("workspace.ui_inspector.minimum_width", value.minimumWidth);
            action("workspace.ui_inspector.minimum_height", value.minimumHeight);
            action("workspace.ui_inspector.maximum_width", value.maximumWidth);
            action("workspace.ui_inspector.maximum_height", value.maximumHeight);
        }

        /** @brief Visit signed and non-negative box edges without a property-name lookup. */
        template <typename T, typename F> void Edges(T &value, F action) {
            action("workspace.ui_inspector.margin_left", value.margin.left);
            action("workspace.ui_inspector.margin_top", value.margin.top);
            action("workspace.ui_inspector.margin_right", value.margin.right);
            action("workspace.ui_inspector.margin_bottom", value.margin.bottom);
            action("workspace.ui_inspector.padding_left", value.padding.left);
            action("workspace.ui_inspector.padding_top", value.padding.top);
            action("workspace.ui_inspector.padding_right", value.padding.right);
            action("workspace.ui_inspector.padding_bottom", value.padding.bottom);
            action("workspace.ui_inspector.border_left", value.border.left);
            action("workspace.ui_inspector.border_top", value.border.top);
            action("workspace.ui_inspector.border_right", value.border.right);
            action("workspace.ui_inspector.border_bottom", value.border.bottom);
            action("workspace.ui_inspector.offset_left", value.offsets.left);
            action("workspace.ui_inspector.offset_top", value.offsets.top);
            action("workspace.ui_inspector.offset_right", value.offsets.right);
            action("workspace.ui_inspector.offset_bottom", value.offsets.bottom);
        }

        /** @brief Visit exact integral layout fields without lossy float conversion. */
        template <typename T, typename F> void LayoutIntegers(T &value, F action) {
            action("workspace.ui_inspector.aspect_width", value.aspectRatio.width);
            action("workspace.ui_inspector.aspect_height", value.aspectRatio.height);
            action("workspace.ui_inspector.pivot_x", value.pivot.x);
            action("workspace.ui_inspector.pivot_y", value.pivot.y);
            action("workspace.ui_inspector.gap", value.container.gap);
            action("workspace.ui_inspector.column_count", value.container.columnCount);
            action("workspace.ui_inspector.row_count", value.container.rowCount);
            action("workspace.ui_inspector.flex_grow", value.flex.grow);
            action("workspace.ui_inspector.flex_shrink", value.flex.shrink);
            action("workspace.ui_inspector.grid_column", value.grid.column);
            action("workspace.ui_inspector.grid_row", value.grid.row);
            action("workspace.ui_inspector.column_span", value.grid.columnSpan);
            action("workspace.ui_inspector.row_span", value.grid.rowSpan);
        }

        /** @brief Visit literal integer fields without changing category or resolving asset identities. */
        template <typename T, typename F> void StyleIntegers(T &value, F action) {
            using V = std::remove_cv_t<T>;
            if constexpr (std::is_same_v<V, UiStyleDimension> || std::is_same_v<V, UiStyleEnumValue>) {
                action("workspace.ui_inspector.value", value.value);
            } else if constexpr (std::is_same_v<V, UiStyleTypography>) {
                action("workspace.ui_inspector.weight", value.weight);
                action("workspace.ui_inspector.stretch", value.stretch);
                action("workspace.ui_inspector.font_style", value.style);
                action("workspace.ui_inspector.font_size", value.size);
                action("workspace.ui_inspector.line_height", value.lineHeight);
                action("workspace.ui_inspector.letter_spacing", value.letterSpacing);
            } else if constexpr (std::is_same_v<V, UiStyleImage>) {
                action("workspace.ui_inspector.slice_left", value.nineSlice[0]);
                action("workspace.ui_inspector.slice_top", value.nineSlice[1]);
                action("workspace.ui_inspector.slice_right", value.nineSlice[2]);
                action("workspace.ui_inspector.slice_bottom", value.nineSlice[3]);
            } else if constexpr (std::is_same_v<V, UiStyleShape>) {
                action("workspace.ui_inspector.radius", value.radius);
                action("workspace.ui_inspector.border_width", value.borderWidth);
                action("workspace.ui_inspector.outline_width", value.outlineWidth);
                action("workspace.ui_inspector.shadow_x", value.shadowOffsetX);
                action("workspace.ui_inspector.shadow_y", value.shadowOffsetY);
                action("workspace.ui_inspector.shadow_blur", value.shadowBlur);
            } else if constexpr (std::is_same_v<V, UiStyleMotionReference>) {
                action("workspace.ui_inspector.duration", value.durationMicroseconds);
                action("workspace.ui_inspector.easing", value.easing);
            }
        }

        /** @brief Resolve only localized enum labels while preserving canonical numeric order. */
        template <typename E, std::size_t N>
        bool Enum(const EditorGuiContext &context, const char *key, E &value, const std::array<const char *, N> &keys) {
            std::array<const char *, N> labels{};
            for (std::size_t i = 0; i < N; ++i)
                labels[i] = Label(context, keys[i]);
            int selected = static_cast<int>(value);
            if (!Ui::DrawComboPropRow(Label(context, key), key, selected, labels, context.theme.fonts))
                return false;
            value = static_cast<E>(selected);
            return true;
        }

        /** @brief Canonical closed alignment vocabulary in persisted numeric order. */
        bool Alignment(const EditorGuiContext &context, const char *key, UiLayoutAlignment &value) {
            return Enum(context, key, value,
                        std::array{"workspace.ui_inspector.start", "workspace.ui_inspector.center", "workspace.ui_inspector.end",
                                   "workspace.ui_inspector.stretch_fit"});
        }

        /** @brief Explicit optional anchor; disabling clears only this detached value and its text. */
        bool Anchor(const EditorGuiContext &context, const char *key, std::optional<UiScalar> &value, UiInspectorInteger &text) {
            ImGui::PushID(key);
            bool enabled = value.has_value();
            bool changed = Ui::CheckboxControl(Label(context, key), &enabled, context.theme.fonts);
            if (changed) {
                if (enabled)
                    value = 0;
                else
                    value.reset();
                text.Capture(0);
            }
            if (value)
                changed = Integer(context, "workspace.ui_inspector.normalized_value", *value, text) || changed;
            ImGui::PopID();
            return changed;
        }

        /** @brief Only active tracks draw; every fixed slot retains stable text offsets. */
        bool Tracks(const EditorGuiContext &context, const char *key, std::array<UiGridTrack, MaximumUiGridTracks> &tracks,
                    const std::uint16_t count, Buffers &buffers, std::size_t &i) {
            bool changed{};
            ImGui::PushID(key);
            Ui::FieldLabel(Label(context, key), context.theme.fonts);
            for (std::size_t track = 0; track < tracks.size(); ++track) {
                if (track < count) {
                    auto &value = tracks[track];
                    ImGui::PushID(static_cast<int>(track));
                    changed = Enum(context, "workspace.ui_inspector.track_kind", value.kind,
                                   std::array{"workspace.ui_inspector.auto", "workspace.ui_inspector.dip", "workspace.ui_inspector.percent",
                                              "workspace.ui_inspector.fraction"}) ||
                              changed;
                    changed = Integer(context, "workspace.ui_inspector.track_value", value.value, buffers.integers[i]) || changed;
                    changed = Length(context, "workspace.ui_inspector.track_minimum", value.minimum, buffers.integers[i + 1]) || changed;
                    changed = Length(context, "workspace.ui_inspector.track_maximum", value.maximum, buffers.integers[i + 2]) || changed;
                    ImGui::PopID();
                }
                i += 3;
            }
            ImGui::PopID();
            return changed;
        }

        /** @brief Draw orthogonal layout policies without a string-to-property mapping. */
        bool LayoutPolicy(const EditorGuiContext &context, UiLayoutStyle &value) {
            bool changed = Alignment(context, "workspace.ui_inspector.horizontal_alignment", value.horizontalAlignment);
            changed = Alignment(context, "workspace.ui_inspector.vertical_alignment", value.verticalAlignment) || changed;
            changed = Enum(context, "workspace.ui_inspector.positioning", value.positioning,
                           std::array{"workspace.ui_inspector.flow", "workspace.ui_inspector.absolute"}) ||
                      changed;
            changed = Enum(context, "workspace.ui_inspector.container_kind", value.container.kind,
                           std::array{"workspace.ui_inspector.stack", "workspace.ui_inspector.flex", "workspace.ui_inspector.grid"}) ||
                      changed;
            changed = Enum(context, "workspace.ui_inspector.orientation", value.container.orientation,
                           std::array{"workspace.ui_inspector.horizontal", "workspace.ui_inspector.vertical"}) ||
                      changed;
            changed = Enum(context, "workspace.ui_inspector.wrap", value.container.wrap,
                           std::array{"workspace.ui_inspector.no_wrap", "workspace.ui_inspector.wrap_enabled"}) ||
                      changed;
            changed = Enum(context, "workspace.ui_inspector.main_alignment", value.container.mainAlignment,
                           std::array{"workspace.ui_inspector.start", "workspace.ui_inspector.center", "workspace.ui_inspector.end",
                                      "workspace.ui_inspector.space_between", "workspace.ui_inspector.space_around",
                                      "workspace.ui_inspector.space_evenly"}) ||
                      changed;
            return Alignment(context, "workspace.ui_inspector.cross_alignment", value.container.crossAlignment) || changed;
        }

        /** @brief Edit linear RGBA including semantic role through shared controls. */
        bool Color(const EditorGuiContext &context, UiStyleColor &value) {
            bool changed =
                Ui::DrawFloatPropRow(Label(context, "workspace.ui_inspector.red"), "red", value.red, context.theme.fonts).changed;
            changed =
                Ui::DrawFloatPropRow(Label(context, "workspace.ui_inspector.green"), "green", value.green, context.theme.fonts).changed ||
                changed;
            changed =
                Ui::DrawFloatPropRow(Label(context, "workspace.ui_inspector.blue"), "blue", value.blue, context.theme.fonts).changed ||
                changed;
            changed =
                Ui::DrawFloatPropRow(Label(context, "workspace.ui_inspector.alpha"), "alpha", value.alpha, context.theme.fonts).changed ||
                changed;
            return Enum(context, "workspace.ui_inspector.color_role", value.role,
                        std::array{"workspace.ui_inspector.generic", "workspace.ui_inspector.text", "workspace.ui_inspector.surface",
                                   "workspace.ui_inspector.border", "workspace.ui_inspector.accent", "workspace.ui_inspector.status"}) ||
                   changed;
        }

        /** @brief Edit an explicit stable asset ID; unresolved resources remain the host validator's responsibility. */
        bool Asset(const EditorGuiContext &context, Assets::AssetId &value, Buffers &buffers) {
            Ui::FieldLabel(Label(context, "workspace.ui_inspector.asset"), context.theme.fonts);
            if (std::find(buffers.asset.begin(), buffers.asset.end(), '\0') == buffers.asset.end()) {
                buffers.asset.back() = '\0';
                buffers.assetValid = false;
            }
            if (!Ui::InputTextControl("asset", buffers.asset.data(), buffers.asset.size(), context.theme.fonts,
                                      {.error = !buffers.assetValid}))
                return false;
            const auto parsed = Assets::AssetId::Parse(std::string_view{buffers.asset.data()});
            buffers.assetValid = parsed.HasValue();
            if (buffers.assetValid)
                value = parsed.Value();
            return true;
        }
    }  // namespace

    /** @copydoc Buffers::IsValid */
    bool Buffers::IsValid() const noexcept {
        return assetValid && std::ranges::all_of(integers, [](const auto &text) {
            return text.valid;
        });
    }

    /** @copydoc CaptureLayout */
    void CaptureLayout(const Runtime::Ui::UiLayoutStyle &value, Buffers &buffers) {
        static_assert(6 + 16 + 13 + 4 + 2 * MaximumUiGridTracks * 3 <= std::tuple_size_v<decltype(Buffers::integers)>);
        buffers = {};
        std::size_t i{};
        Sizes(value, [&](const char *, const auto &length) {
            buffers.integers[i++].Capture(length.value);
        });
        Edges(value, [&](const char *, const auto edge) {
            buffers.integers[i++].Capture(edge);
        });
        LayoutIntegers(value, [&](const char *, const auto integer) {
            buffers.integers[i++].Capture(integer);
        });
        for (const auto &axis : {value.anchors.horizontal, value.anchors.vertical}) {
            buffers.integers[i++].Capture(axis.start.value_or(0));
            buffers.integers[i++].Capture(axis.end.value_or(0));
        }
        for (const auto *tracks : {&value.container.columns, &value.container.rows}) {
            for (const auto &track : *tracks) {
                buffers.integers[i++].Capture(track.value);
                buffers.integers[i++].Capture(track.minimum.value);
                buffers.integers[i++].Capture(track.maximum.value);
            }
        }
    }

    /** @copydoc Layout */
    bool Layout(const EditorGuiContext &context, Runtime::Ui::UiLayoutStyle &value, Buffers &buffers) {
        bool changed{};
        std::size_t i{};
        Sizes(value, [&](const char *key, auto &length) {
            changed = Length(context, key, length, buffers.integers[i++]) || changed;
        });
        Edges(value, [&](const char *key, auto &edge) {
            changed = Integer(context, key, edge, buffers.integers[i++]) || changed;
        });
        LayoutIntegers(value, [&](const char *key, auto &integer) {
            changed = Integer(context, key, integer, buffers.integers[i++]) || changed;
        });
        changed = Anchor(context, "workspace.ui_inspector.anchor_left", value.anchors.horizontal.start, buffers.integers[i++]) || changed;
        changed = Anchor(context, "workspace.ui_inspector.anchor_right", value.anchors.horizontal.end, buffers.integers[i++]) || changed;
        changed = Anchor(context, "workspace.ui_inspector.anchor_top", value.anchors.vertical.start, buffers.integers[i++]) || changed;
        changed = Anchor(context, "workspace.ui_inspector.anchor_bottom", value.anchors.vertical.end, buffers.integers[i++]) || changed;
        changed = Alignment(context, "workspace.ui_inspector.anchor_horizontal", value.anchors.horizontal.alignment) || changed;
        changed = Alignment(context, "workspace.ui_inspector.anchor_vertical", value.anchors.vertical.alignment) || changed;
        changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.preserve_width"), &value.anchors.horizontal.preserveSize,
                                      context.theme.fonts) ||
                  changed;
        changed = Ui::CheckboxControl(Label(context, "workspace.ui_inspector.preserve_height"), &value.anchors.vertical.preserveSize,
                                      context.theme.fonts) ||
                  changed;
        changed = LayoutPolicy(context, value) || changed;
        changed =
            Tracks(context, "workspace.ui_inspector.columns", value.container.columns, value.container.columnCount, buffers, i) || changed;
        changed = Tracks(context, "workspace.ui_inspector.rows", value.container.rows, value.container.rowCount, buffers, i) || changed;
        return changed;
    }

    /** @copydoc CaptureStyle */
    void CaptureStyle(const Runtime::Ui::UiStyleValue &value, Buffers &buffers) {
        buffers = {};
        std::size_t i{};
        std::visit([&](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            StyleIntegers(typed, [&](const char *, const auto integer) {
                buffers.integers[i++].Capture(integer);
            });
            if constexpr (std::is_same_v<T, UiStyleTypography> || std::is_same_v<T, UiStyleImage>) {
                const auto &id = [&]() -> const Assets::AssetId & {
                    if constexpr (std::is_same_v<T, UiStyleTypography>)
                        return typed.family;
                    else
                        return typed.asset;
                }();
                const auto text = id.ToString();
                if (text.size() >= buffers.asset.size())
                    buffers.assetValid = false;
                else
                    std::copy(text.begin(), text.end(), buffers.asset.begin());
            }
        }, value);
    }

    /** @copydoc Style */
    bool Style(const EditorGuiContext &context, Runtime::Ui::UiStyleValue &value, Buffers &buffers) {
        bool changed{};
        std::size_t i{};
        std::visit([&](auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            StyleIntegers(typed, [&](const char *key, auto &integer) {
                changed = Integer(context, key, integer, buffers.integers[i++]) || changed;
            });
            if constexpr (std::is_same_v<T, UiStyleColor>)
                changed = Color(context, typed) || changed;
            else if constexpr (std::is_same_v<T, UiStyleScalar>)
                changed = Ui::DrawFloatPropRow(Label(context, "workspace.ui_inspector.value"), "scalar", typed.value, context.theme.fonts)
                              .changed ||
                          changed;
            else if constexpr (std::is_same_v<T, UiStyleTypography>)
                changed = Asset(context, typed.family, buffers) || changed;
            else if constexpr (std::is_same_v<T, UiStyleImage>) {
                changed = Asset(context, typed.asset, buffers) || changed;
                changed = Enum(context, "workspace.ui_inspector.image_fit", typed.fit,
                               std::array{"workspace.ui_inspector.stretch_fit", "workspace.ui_inspector.contain",
                                          "workspace.ui_inspector.cover", "workspace.ui_inspector.tile"}) ||
                          changed;
                changed = Color(context, typed.tint) || changed;
            }
        }, value);
        return changed;
    }
}  // namespace Horo::Editor::UiInspectorWidgets
