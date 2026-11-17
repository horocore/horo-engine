#include "NavigationOverlayLayer.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorViewportScene.h"
#include "editor/screens/workspace/panels/viewport/ViewportOverlay.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        constexpr char OptionsPopup[] = "##NavigationOverlayOptions";

        /** @brief Resolves all copy through the editor localization authority. */
        const std::string &Text(const EditorGuiContext &context, const char *key) {
            constexpr std::string_view prefix{"workspace.navigation_overlay."};
            std::array<char, 128> fullKey{};
            const std::string_view suffix{key};
            std::ranges::copy(prefix, fullKey.begin());
            std::ranges::copy(suffix, fullKey.begin() + prefix.size());
            return context.localization.Get("editor", std::string_view{fullKey.data(), prefix.size() + suffix.size()});
        }

        /** @brief A fixed polygon scratch covers a triangle clipped to all six homogeneous planes. */
        struct Clipped final {
            std::array<Math::Vec4, 12> points{};
            std::size_t count{};
        };

        /** @brief Signed homogeneous frustum distance, supporting both peer depth conventions. */
        float Distance(const Math::Vec4 p, const unsigned plane, const Math::ClipDepthRange depth) {
            switch (plane) {
                case 0:
                    return p.w + p.x;
                case 1:
                    return p.w - p.x;
                case 2:
                    return p.w + p.y;
                case 3:
                    return p.w - p.y;
                case 4:
                    return depth == Math::ClipDepthRange::NegativeOneToOne ? p.w + p.z : p.z;
                default:
                    return p.w - p.z;
            }
        }

        /** @brief Clips before perspective division, preventing near-plane blowups and lost crossing triangles. */
        Clipped Clip(Clipped input, const bool triangle, const Math::ClipDepthRange depth) {
            for (unsigned plane = 0; plane < 6 && input.count; ++plane) {
                Clipped output;
                const std::size_t edges = triangle ? input.count : input.count - 1;
                for (std::size_t i = 0; i < edges; ++i) {
                    const auto a = input.points[i];
                    const auto b = input.points[(i + 1) % input.count];
                    const float da = Distance(a, plane, depth);
                    const float db = Distance(b, plane, depth);
                    if (da >= 0 && output.count < output.points.size())
                        output.points[output.count++] = a;
                    if ((da >= 0) != (db >= 0) && output.count < output.points.size())
                        output.points[output.count++] = a + (b - a) * (da / (da - db));
                    if (!triangle && db >= 0 && output.count < output.points.size())
                        output.points[output.count++] = b;
                }
                input = output;
                if (!triangle && input.count < 2)
                    return {};
            }
            return input;
        }

        /** @brief Tokens are resolved each frame, so theme/accessibility changes recolor existing snapshots. */
        ImU32 Color(const NavigationOverlayPrimitive &primitive) {
            if (primitive.validity == NavigationOverlayValidity::Disabled)
                return Theme::U32(Theme::Muted());
            if (primitive.validity == NavigationOverlayValidity::Unvalidated)
                return Theme::U32(Theme::Warn());
            switch (primitive.kind) {
                using enum NavigationOverlayKind;
                case Surface:
                    return Theme::U32(Theme::Accent());
                case Walkable:
                    return Theme::U32(Theme::Mix(Theme::Ok(), Theme::Text(), 0.15F));
                case Tile:
                    return Theme::U32(Theme::Text());
                case Modifier:
                    return Theme::U32(Theme::Warn());
                case Link:
                    return Theme::U32(Theme::Accent());
            }
            return Theme::U32(Theme::Text());
        }

        /** @brief ImGui convex AA requires clockwise screen winding, including back-facing navigation geometry. */
        void Clockwise(std::array<ImVec2, 12> &pixels, const std::size_t count) {
            float winding{};
            for (std::size_t i = 0; i < count; ++i) {
                const auto a = pixels[i];
                const auto b = pixels[(i + 1) % count];
                winding += a.x * b.y - b.x * a.y;
            }
            if (winding < 0)
                std::reverse(pixels.begin(), pixels.begin() + count);
        }

        /** @brief Emits an already clipped primitive with shape cues and a strict draw-command ceiling. */
        void DrawPrimitive(ImDrawList &draw, const NavigationOverlayPrimitive &primitive, const Math::Mat4 &matrix, const ImVec2 origin,
                           const ImVec2 size, const Math::ClipDepthRange depth, std::size_t &commands) {
            const auto projected = ProjectNavigationOverlay(primitive, matrix, {origin.x, origin.y}, {size.x, size.y}, depth);
            if (!projected.count)
                return;
            std::array<ImVec2, 12> pixels{};
            for (std::size_t i = 0; i < projected.count; ++i)
                pixels[i] = {projected.points[i].x, projected.points[i].y};
            const auto color = Color(primitive);
            if (primitive.triangle) {
                Clockwise(pixels, projected.count);
                // Filled faces and outlined boxes/links remain distinguishable without color alone.
                draw.AddConvexPolyFilled(pixels.data(), static_cast<int>(projected.count),
                                         (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 55));
                ++commands;
                return;
            }
            draw.AddLine(pixels[0], pixels[1], color, primitive.kind == NavigationOverlayKind::Modifier ? 2.5F : 1.5F);
            ++commands;
            if (primitive.kind != NavigationOverlayKind::Link || commands + 4 > 1024)
                return;
            const auto arrow = [&](const ImVec2 from, const ImVec2 to) {
                const float dx = to.x - from.x;
                const float dy = to.y - from.y;
                const float length = std::hypot(dx, dy);
                if (length <= 1.0F)
                    return;
                const float x = dx / length;
                const float y = dy / length;
                draw.AddLine(to, {to.x - x * 8.0F + y * 4.0F, to.y - y * 8.0F - x * 4.0F}, color, 2.0F);
                draw.AddLine(to, {to.x - x * 8.0F - y * 4.0F, to.y - y * 8.0F + x * 4.0F}, color, 2.0F);
                commands += 2;
            };
            arrow(pixels[0], pixels[1]);
            if (primitive.bidirectional)
                arrow(pixels[1], pixels[0]);
        }

        /** @brief Applies independent inspection/draw bounds even when every primitive is filtered out. */
        bool DrawScene(const NavigationOverlayScene &scene, const NavigationOverlayFilter &filter, const NavigationOverlayFrame &frame) {
            if (const auto matrix = BuildEditorViewportViewProjection(frame.camera, frame.size.x / frame.size.y, frame.depthRange);
                matrix.HasValue()) {
                auto &draw = *ImGui::GetWindowDrawList();
                draw.PushClipRect(frame.origin, {frame.origin.x + frame.size.x, frame.origin.y + frame.size.y}, true);
                std::size_t inspected{};
                std::size_t commands{};
                for (const auto &primitive : scene.primitives) {
                    if (inspected++ >= 16384 || commands >= 1024) {
                        draw.PopClipRect();
                        return true;
                    }
                    if (filter.Matches(primitive))
                        DrawPrimitive(draw, primitive, matrix.Value(), frame.origin, frame.size, frame.depthRange, commands);
                }
                draw.PopClipRect();
            }
            return false;
        }

    }  // namespace

    /** @copydoc ProjectNavigationOverlay */
    ProjectedNavigationOverlay ProjectNavigationOverlay(const NavigationOverlayPrimitive &primitive, const Math::Mat4 &matrix,
                                                        const Math::Vec2 origin, const Math::Vec2 extent,
                                                        const Math::ClipDepthRange depth) noexcept {
        if (!Math::IsFinite(origin) || !Math::IsFinite(extent) || extent.x <= 0 || extent.y <= 0)
            return {};
        Clipped clip;
        clip.count = primitive.triangle ? 3 : 2;
        for (std::size_t i = 0; i < clip.count; ++i) {
            const auto p = primitive.points[i];
            clip.points[i] = Math::TransformHomogeneous(matrix, {p.x, p.y, p.z, 1.0F});
            if (!Math::IsFinite(clip.points[i]))
                return {};
        }
        clip = Clip(clip, primitive.triangle, depth);
        if (clip.count < (primitive.triangle ? 3U : 2U))
            return {};
        ProjectedNavigationOverlay output;
        output.count = clip.count;
        for (std::size_t i = 0; i < clip.count; ++i) {
            const auto p = clip.points[i];
            if (p.w <= Math::DefaultEpsilon)
                return {};
            output.points[i] = {origin.x + (p.x / p.w * 0.5F + 0.5F) * extent.x, origin.y + (0.5F - p.y / p.w * 0.5F) * extent.y};
            if (!Math::IsFinite(output.points[i]))
                return {};
        }
        return output;
    }

    /** @copydoc NavigationOverlayLayer::Choices */
    void NavigationOverlayLayer::Choices(const NavigationOverlayScene &scene) {
        for (auto &choices : choices_)
            choices.clear();
        for (const auto &primitive : scene.primitives) {
            const std::array values{primitive.profile.Value(), primitive.area.Value(), primitive.surface.Value()};
            for (std::size_t i = 0; i < values.size(); ++i)
                if (values[i])
                    choices_[i].push_back({values[i], {}});
            if (primitive.otherSurface.IsValid())
                choices_[2].push_back({primitive.otherSurface.Value(), {}});
        }
        for (auto &choices : choices_) {
            std::ranges::sort(choices, {}, &Choice::value);
            choices.erase(std::ranges::unique(choices, {}, &Choice::value).begin(), choices.end());
            for (auto &choice : choices)
                choice.label = std::to_string(choice.value);
        }
        selected_ = {};
        filter_.profile.reset();
        filter_.area.reset();
        filter_.surface.reset();
    }

    /** @copydoc NavigationOverlayLayer::ApplyIdentityFilter */
    void NavigationOverlayLayer::ApplyIdentityFilter(const std::size_t field) {
        const auto value = selected_[field] ? choices_[field][selected_[field] - 1].value : 0;
        if (field == 0)
            filter_.profile =
                value ? std::optional{GuiLive::HostSymbol<&Navigation::NavigationAgentProfileId::Create>()(value).Value()} : std::nullopt;
        else if (field == 1)
            filter_.area =
                value ? std::optional{GuiLive::HostSymbol<&Navigation::NavigationAreaId::Create>()(value).Value()} : std::nullopt;
        else
            filter_.surface = value ? std::optional{GuiLive::HostSymbol<&Navigation::SurfaceId::Create>()(value).Value()} : std::nullopt;
    }

    /** @copydoc NavigationOverlayLayer::IdentityControl */
    void NavigationOverlayLayer::IdentityControl(const EditorGuiContext &context, const std::size_t field, const char *key) {
        Ui::Hint(Text(context, key).c_str(), context.theme.fonts);
        ImGui::PushID(static_cast<int>(field));
        if (const Ui::ComboItemSource source{.label =
                                                 [this, &context, field](const int row) {
            return row == 0 ? Text(context, "all").c_str() : choices_[field][row - 1].label.c_str();
        }};
            Ui::ComboControl("##Filter", &selected_[field], static_cast<int>(choices_[field].size() + 1), source, context.theme.fonts))
            ApplyIdentityFilter(field);
        ImGui::PopID();
    }

    /** @copydoc NavigationOverlayLayer::Controls */
    void NavigationOverlayLayer::Controls(const EditorGuiContext &context) {
        constexpr std::array keys{"profile", "area", "surface"};
        for (std::size_t i = 0; i < keys.size(); ++i)
            IdentityControl(context, i, keys[i]);
        const std::array validity{Text(context, "all").c_str(), Text(context, "authored").c_str(), Text(context, "cooked").c_str(),
                                  Text(context, "disabled").c_str(), Text(context, "unvalidated").c_str()};
        int selected = filter_.validity ? static_cast<int>(*filter_.validity) + 1 : 0;
        Ui::Hint(Text(context, "validity").c_str(), context.theme.fonts);
        if (Ui::ComboControl("##Validity", &selected, validity.data(), static_cast<int>(validity.size()), context.theme.fonts))
            filter_.validity = selected ? std::optional{static_cast<NavigationOverlayValidity>(selected - 1)} : std::nullopt;
        constexpr std::array kinds{"bounds", "walkable", "tiles", "modifiers", "links"};
        for (std::size_t i = 0; i < kinds.size(); ++i)
            static_cast<void>(Ui::CheckboxControl(Text(context, kinds[i]).c_str(), &filter_.kinds[i], context.theme.fonts));
    }

    /** @copydoc NavigationOverlayLayer::Draw */
    void NavigationOverlayLayer::Draw(const NavigationViewportAccess &access, const NavigationOverlayFrame &frame) {
        const auto size = frame.size;
        capturesPointer_ = false;
        if (size.x <= 32 || size.y <= 180 || !std::isfinite(size.x) || !std::isfinite(size.y))
            return;
        ImGui::PushID("NavigationOverlays");
        const auto snapshot = (visible_ || ImGui::IsPopupOpen(OptionsPopup)) && access.query ? access.query(frame.projectRoot) : nullptr;
        const NavigationOverlayScene *scene{};
        if (snapshot)
            for (const auto &candidate : snapshot->Scenes())
                if (candidate.path == frame.scenePath) {
                    scene = &candidate;
                    break;
                }
        if (scene && (observed_.lock() != snapshot || scenePath_ != frame.scenePath)) {
            Choices(*scene);
            observed_ = snapshot;
            scenePath_ = frame.scenePath;
        }
        const bool frameLimited = visible_ && scene && DrawScene(*scene, filter_, frame);
        DrawOptions(access, snapshot, scene, frame, frameLimited);
        ImGui::PopID();
    }

    /** @copydoc NavigationOverlayLayer::DrawOptions */
    void NavigationOverlayLayer::DrawOptions(const NavigationViewportAccess &access,
                                             const std::shared_ptr<const NavigationViewportSnapshot> &snapshot,
                                             const NavigationOverlayScene *scene, const NavigationOverlayFrame &frame,
                                             const bool frameLimited) {
        const auto &context = frame.gui;
        const auto origin = frame.origin;
        const auto size = frame.size;
        const auto &tokens = Theme::GetActiveTokens();
        const float scale = tokens.sizes.uiScale;
        ImGui::SetCursorScreenPos(ResolveViewportToolbar(origin, size).optionsPosition);
        if (Ui::Button(
                {.label = Text(context, "title").c_str(), .size = {std::min(220.0F, size.x - 32), 0}, .font = context.theme.fonts.sans}))
            ImGui::OpenPopup(OptionsPopup);
        capturesPointer_ = ImGui::IsItemHovered() || ImGui::IsPopupOpen(OptionsPopup);
        const float availableWidth = std::max(1.0F, size.x - 32.0F);
        ImGui::SetNextWindowSizeConstraints({std::min(240.0F * scale, availableWidth), 0.0F},
                                            {std::min(360.0F * scale, availableWidth), std::max(180.0F, size.y - 32.0F)});
        const float padding = DesignSystem::SpacingFor(tokens, Ui::SpacingSize::Medium);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padding, padding});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {tokens.spacing.propertyRowGap, tokens.spacing.propertyRowGap});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, tokens.radii.card);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, scale);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::MenuSurface());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::MenuBorder());
        if (ImGui::BeginPopup(OptionsPopup)) {
            static_cast<void>(Ui::CheckboxControl(Text(context, "show").c_str(), &visible_, context.theme.fonts));
            Ui::ContextMenuSeparator();
            if (visible_ && !prepared_ && access.prepare) {
                auto prepared = access.prepare(frame.projectRoot);
                prepared_ = prepared.HasValue();
            }
            if (scene)
                Controls(context);
            else
                Ui::Hint(Text(context, "unavailable").c_str(), context.theme.fonts);
            Ui::Hint(Text(context, frame.dirty ? "dirty" : "saved").c_str(), context.theme.fonts);
            if (frameLimited || (snapshot && snapshot->Truncated()))
                Ui::Hint(Text(context, "limited").c_str(), context.theme.fonts);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
    }
}  // namespace Horo::Editor
