#pragma once
/** @file NavigationOverlayLayer.h
 * @brief Presentation-only navigation controls and bounded neutral geometry drawing.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @brief Fixed-size projected polygon or segment after clipping; no per-frame allocation. */
    struct ProjectedNavigationOverlay final {
        std::array<Math::Vec2, 12> points{};
        std::size_t count{};
    };

    /** @brief Clips owned world geometry against all six planes before perspective division.
     * @param primitive Owned line or triangle. @param matrix Canonical peer view-projection matrix.
     * @param origin Viewport pixel origin. @param extent Positive finite pixel extent.
     * @param depth Peer clip-depth convention. @return Empty when invisible or invalid, otherwise bounded pixel geometry.
     */
    [[nodiscard]] ProjectedNavigationOverlay ProjectNavigationOverlay(const NavigationOverlayPrimitive &primitive, const Math::Mat4 &matrix,
                                                                      Math::Vec2 origin, Math::Vec2 extent,
                                                                      Math::ClipDepthRange depth) noexcept;

    /** @brief Panel-owned visibility and identity choices; no provider memory survives a draw. */
    class NavigationOverlayLayer final {
    public:
        /** @brief Queries memory-only owner projection, then draws clipped geometry and shared controls.
         * @param access Explicit host capability, outliving attachment. @param root Exact active project.
         * @param scenePath Exact active Scene namespace. @param camera Canonical viewport camera.
         * @param origin Viewport top-left. @param size Positive viewport extent.
         * @param depthRange Concrete peer's clip convention. @param context Current tokens/fonts/localization.
         * @param dirty Whether saved geometry differs from the editable document.
         */
        void Draw(const NavigationBakeWorkspaceAccess &access, const std::filesystem::path &root, const std::filesystem::path &scenePath,
                  const EditorViewportCamera &camera, ImVec2 origin, ImVec2 size, Math::ClipDepthRange depthRange,
                  const EditorGuiContext &context, bool dirty);

        /** @brief Whether the options button or popup owns pointer interaction this frame. @return Presentation capture. */
        [[nodiscard]] bool CapturesPointer() const noexcept {
            return capturesPointer_;
        }

    private:
        /** @brief Cached stable identity label, recomputed only on publication replacement. */
        struct Choice final {
            std::uint64_t value{};
            std::string label;
        };

        /** @brief Caches bounded identity choices only when immutable Scene evidence changes. @param scene Exact Scene. */
        void Choices(const NavigationOverlayScene &scene);
        /** @brief Draws shared localized filter controls. @param context Current presentation tokens. */
        void Controls(const EditorGuiContext &context);
        NavigationOverlayFilter filter_;
        std::array<std::vector<Choice>, 3> choices_;
        std::array<int, 3> selected_{};
        std::weak_ptr<const NavigationViewportSnapshot> observed_;
        std::filesystem::path scenePath_;
        bool capturesPointer_{};
        bool visible_{};
        bool prepared_{};
    };
}  // namespace Horo::Editor
