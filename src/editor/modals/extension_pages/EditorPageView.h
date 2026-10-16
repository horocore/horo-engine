#pragma once
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Extensions/EditorPageHost.h"

#include <array>

namespace Horo::Editor {
    /** @brief Actual shared editor FormKit adapter; retains only bounded edit buffers and copied page identity between frames. */
    class EditorPageView final {
    public:
        EditorPageView(const EditorGuiContext &context, std::shared_ptr<Extensions::EditorPageHost> host,
                       std::shared_ptr<const Extensions::EditorThemeFrame> theme);
        /** @brief Shows registered settings contributions and edits the selected page through the canonical page controller. */
        void DrawSettings();
        /** @brief Selects one exact already-open modal session without restarting its draft. */
        void Select(Extensions::EditorSurfaceProviderIdentity provider, std::string id);
        /** @brief Draws the current actual prepared page and host-owned Apply/Reset/Cancel controls. */
        void DrawPage();
        /** @brief Cancels the selected draft and requests serialized provider cancellation. */
        void Cancel() noexcept;
        /** @brief Returns the exact current projection, or null after provider removal. */
        const Extensions::EditorPageProjection *Current() const noexcept;

    private:
        /** @brief Resolves copied provider text through the host, never a module callback. */
        const char *Text(std::string_view key) const;
        /** @brief Rebuilds bounded text buffers only after an actual page revision change. */
        void Synchronize(const Extensions::EditorPageProjection &page);
        /** @brief Caches the validated common FormKit projection only for the actual page/theme/width generation. */
        bool PrepareRender(const Extensions::EditorPageProjection &page);
        /** @brief Uses the validated common projection geometry; localized labels can expand vertical slots without overlapping controls.
         */
        void DrawForm(const Extensions::EditorPageProjection &page);
        /** @brief Orders the cached vertical profile by validated geometry/ancestry without allocating during normal drawing. */
        void OrderRender(const Extensions::EditorPageProjection &page);
        /** @brief Draws one admitted standard primitive with exact typed owner mutation. */
        void DrawNode(const Extensions::EditorUiNode &node, const Extensions::EditorPageProjection &page, std::size_t index);
        /** @brief Handles one actual control edit through exact typed values; no raw module callbacks run during drawing. */
        void DrawValue(const Extensions::EditorUiNode &node, const Extensions::EditorPageProjection &page, std::size_t index);
        /** @brief Retains original controller errors for explicit presentation without altering canonical page state. */
        void Accept(Result<void> result);
        const EditorGuiContext &context_;
        std::shared_ptr<Extensions::EditorPageHost> host_;
        std::shared_ptr<const Extensions::EditorThemeFrame> theme_;
        std::optional<Extensions::EditorUiRenderSnapshot> render_;
        std::uint64_t renderRevision_{};
        std::array<std::size_t, 256> renderOrder_{};
        Extensions::EditorSurfaceProviderIdentity provider_;
        std::string id_;
        std::uint64_t bufferRevision_{};
        std::array<std::string, 256> buffers_;
        std::optional<Error> failure_;
    };
}  // namespace Horo::Editor
