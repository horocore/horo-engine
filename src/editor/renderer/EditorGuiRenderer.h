#pragma once

/**
 * @file EditorGuiRenderer.h
 * @brief Editor-private backend-neutral GUI renderer contract.
 */

#include "Horo/Runtime/Render/RenderBackend.h"
#include "editor/renderer/EditorViewportRenderer.h"

#include <span>

namespace Horo::Editor {
    /** @brief Borrowed RGBA8 source pixels for one immutable editor UI texture upload. */
    struct EditorRgba8ImageView {
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::span<const std::uint8_t> pixels{};

        /** @brief Returns true when the dimensions and tightly packed pixel payload agree. */
        [[nodiscard]] bool IsValid() const noexcept {
            return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width) * height * 4U;
        }
    };

    /**
     * @brief Owns Dear ImGui platform/renderer bridges and editor UI texture resources.
     *
     * Implementations borrow their presentation attachment and window. Calls are
     * serialized on the editor UI/render thread. No native graphics type crosses
     * this contract.
     */
    class IEditorGuiRenderer : public Render::IGuiPassExecutor {
    public:
        virtual ~IEditorGuiRenderer() = default;

        /** @brief Borrows the viewport's exact current generation for this composition; an empty view removes the previous registration. */
        void BindViewportTexture(const EditorViewportTextureView view) noexcept {
            viewportBinding_ = view;
        }

        /** @brief Initializes Dear ImGui platform and renderer bridges. */
        [[nodiscard]] virtual Result<void> Initialize() = 0;

        /** @brief Prepares renderer and SDL platform state for one active backend frame. */
        [[nodiscard]] virtual Result<void> BeginFrame() = 0;

        /** @brief Encodes current Dear ImGui draw data into the active backend frame. */
        [[nodiscard]] Result<void> RenderDrawData(const Render::GuiCompositionRequest &request) override = 0;

        /** @brief Uploads one immutable tightly packed RGBA8 UI texture. */
        [[nodiscard]] virtual Result<std::uintptr_t> CreateTexture(const EditorRgba8ImageView &image) = 0;

        /** @brief Releases one texture previously created by this renderer. */
        virtual void DestroyTexture(std::uintptr_t textureId) noexcept = 0;

        /** @brief Releases all GUI backend and texture resources; repeated calls are safe. */
        virtual void Shutdown() noexcept = 0;

    protected:
        /** @brief Generation-bound borrowed viewport registration; never a second resource owner. */
        [[nodiscard]] const EditorViewportTextureView &ViewportBinding() const noexcept {
            return viewportBinding_;
        }

    private:
        EditorViewportTextureView viewportBinding_;
    };
}  // namespace Horo::Editor
