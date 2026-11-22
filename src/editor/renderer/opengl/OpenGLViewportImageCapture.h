#pragma once

/** @file OpenGLViewportImageCapture.h @brief OpenGL-owned asynchronous viewport crop readback. */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "editor/renderer/EditorViewportImageCapture.h"

#include <chrono>
#include <functional>
#include <glad/gl.h>
#include <optional>

namespace Horo::Editor {
    /** @brief One native staging buffer and fence on the viewport's render-capable owner lane. */
    class OpenGLViewportImageCapture final {
    public:
        OpenGLViewportImageCapture() = default;

        /** @brief Releases any native copy while the attaching viewport still owns the context. */
        ~OpenGLViewportImageCapture() {
            Shutdown();
        }

        OpenGLViewportImageCapture(const OpenGLViewportImageCapture &) = delete;
        OpenGLViewportImageCapture &operator=(const OpenGLViewportImageCapture &) = delete;

        /** @brief Admits one bounded rectangle and remembers its exact texture generation. */
        [[nodiscard]] Result<std::uint64_t> Request(EditorViewportImageRegion region, Render::RenderTargetHandle target,
                                                    std::uint32_t width, std::uint32_t height, std::function<bool()> authorized = {});
        /** @brief Copies ready artifact ownership without touching native state. */
        [[nodiscard]] std::optional<EditorViewportImageSnapshot> Read(std::uint64_t id);
        /** @brief Prevents a pending copy from publishing without reusing its staging prematurely. */
        bool Cancel(std::uint64_t id) noexcept;
        /** @brief Reports whether a new native copy may need the target framebuffer this frame. */
        [[nodiscard]] bool NeedsFramebuffer() const noexcept;
        /** @brief Advances the request after normal viewport rendering. Never waits for its fence. */
        void Advance(Render::RenderTargetHandle target, std::uint32_t width, std::uint32_t height, bool frameFailed,
                     std::optional<GLuint> renderedFramebuffer);
        /** @brief Releases native names and revokes artifacts while the OpenGL context is still owned. */
        void Shutdown() noexcept;

    private:
        using Clock = std::chrono::steady_clock;

        void Expire(Clock::time_point now) noexcept;
        void WithdrawIfUnauthorized();
        void ReconcileSource(Render::RenderTargetHandle target, std::uint32_t width, std::uint32_t height, bool frameFailed) noexcept;
        bool Submit(GLuint framebuffer);
        void Poll();
        bool CopyPixels();
        void ReleaseCopy() noexcept;

        std::uint64_t serial_{};
        std::optional<Clock::time_point> started_;
        Render::RenderTargetHandle target_;
        std::uint32_t width_{};
        std::uint32_t height_{};
        EditorViewportImageRegion region_;
        EditorViewportImagePhase phase_{EditorViewportImagePhase::Failed};
        std::shared_ptr<const EditorViewportImageArtifact> artifact_;
        std::function<bool()> authorized_;
        GLuint buffer_{};
        GLsync fence_{};
        bool copyFenceUnavailable_{};
    };
}  // namespace Horo::Editor
