#include "OpenGLViewportImageCapture.h"

#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        constexpr auto Lifetime = std::chrono::seconds{30};
        constexpr auto StartSpacing = std::chrono::seconds{1};
        constexpr std::array PackFields{GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS};

        /** @brief Restores the application's pack buffer and rectangle layout. */
        class PixelPackState final {
        public:
            PixelPackState() {
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer_);
                for (std::size_t index = 0; index < PackFields.size(); ++index)
                    glGetIntegerv(PackFields[index], &values_[index]);
            }

            ~PixelPackState() {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(buffer_));
                for (std::size_t index = 0; index < PackFields.size(); ++index)
                    glPixelStorei(PackFields[index], values_[index]);
            }

            PixelPackState(const PixelPackState &) = delete;
            PixelPackState &operator=(const PixelPackState &) = delete;

        private:
            GLint buffer_{};
            std::array<GLint, PackFields.size()> values_{};
        };

        /** @brief Selects only the offscreen color attachment and restores both framebuffer read selectors. */
        class ReadFramebufferState final {
        public:
            explicit ReadFramebufferState(GLuint framebuffer) {
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
                glGetIntegerv(GL_READ_BUFFER, &selected_);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
            }

            ~ReadFramebufferState() {
                glReadBuffer(static_cast<GLenum>(selected_));
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_));
            }

            ReadFramebufferState(const ReadFramebufferState &) = delete;
            ReadFramebufferState &operator=(const ReadFramebufferState &) = delete;

        private:
            GLint previous_{};
            GLint selected_{};
        };

        /** @brief Requires the explicit asynchronous route at the actual context. */
        bool ReadbackAvailable() noexcept {
            return glReadPixels && glFenceSync && glClientWaitSync && glDeleteSync && glMapBufferRange && glUnmapBuffer && glFlush;
        }
    }  // namespace

    /** @copydoc OpenGLViewportImageCapture::Request */
    Result<std::uint64_t> OpenGLViewportImageCapture::Request(EditorViewportImageRegion region, Render::RenderTargetHandle target,
                                                              std::uint32_t width, std::uint32_t height, std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        const auto now = Clock::now();
        Expire(now);
        if (!target.IsValid() || !region.Fits(width, height) || !ReadbackAvailable() || copyFenceUnavailable_)
            return Output::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable));
        if (fence_ || phase_ == EditorViewportImagePhase::Pending || (started_ && now - *started_ < StartSpacing) ||
            serial_ == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable, "Viewport readback is busy."));
        started_ = now;
        target_ = target;
        width_ = width;
        height_ = height;
        region_ = region;
        phase_ = EditorViewportImagePhase::Pending;
        WithdrawPublication();
        authorized_ = std::move(authorized);
        return Output::Success(++serial_);
    }

    /** @copydoc OpenGLViewportImageCapture::Read */
    std::optional<EditorViewportImageSnapshot> OpenGLViewportImageCapture::Read(std::uint64_t id) {
        Expire(Clock::now());
        WithdrawIfUnauthorized();
        if (!started_ || serial_ != id)
            return std::nullopt;
        return EditorViewportImageSnapshot{phase_, artifact_, publication_, *started_ + Lifetime};
    }

    /** @copydoc OpenGLViewportImageCapture::Cancel */
    bool OpenGLViewportImageCapture::Cancel(std::uint64_t id) noexcept {
        Expire(Clock::now());
        if (!id || serial_ != id || (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready))
            return false;
        authorized_ = {};
        WithdrawPublication();
        phase_ = EditorViewportImagePhase::Cancelled;
        return true;
    }

    /** @copydoc OpenGLViewportImageCapture::NeedsFramebuffer */
    bool OpenGLViewportImageCapture::NeedsFramebuffer() const noexcept {
        return phase_ == EditorViewportImagePhase::Pending && !fence_;
    }

    /** @copydoc OpenGLViewportImageCapture::Advance */
    void OpenGLViewportImageCapture::Advance(Render::RenderTargetHandle target, std::uint32_t width, std::uint32_t height, bool frameFailed,
                                             std::optional<GLuint> renderedFramebuffer) {
        if (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready && !fence_)
            return;
        Expire(Clock::now());
        ReconcileSource(target, width, height, frameFailed);
        WithdrawIfUnauthorized();
        if (fence_) {
            Poll();
            return;
        }
        if (NeedsFramebuffer() && renderedFramebuffer && !Submit(*renderedFramebuffer))
            phase_ = EditorViewportImagePhase::Failed;
    }

    /** @brief Withdraws publication when the exact render target changes or fails. */
    void OpenGLViewportImageCapture::ReconcileSource(Render::RenderTargetHandle target, std::uint32_t width, std::uint32_t height,
                                                     bool frameFailed) noexcept {
        if (phase_ != EditorViewportImagePhase::Pending && phase_ != EditorViewportImagePhase::Ready)
            return;
        const bool resized = target != target_ || width != width_ || height != height_;
        if (resized || frameFailed) {
            WithdrawPublication();
            phase_ = resized ? EditorViewportImagePhase::Resized : EditorViewportImagePhase::Failed;
        }
    }

    /** @brief Expires only publication so already queued GPU work can retire without waiting. */
    void OpenGLViewportImageCapture::Expire(Clock::time_point now) noexcept {
        if (started_ && now - *started_ >= Lifetime) {
            WithdrawPublication();
            phase_ = EditorViewportImagePhase::Expired;
            authorized_ = {};
        }
    }

    /** @brief Revokes both pending and detached image evidence when the owner leaves its admitted scope. */
    void OpenGLViewportImageCapture::WithdrawIfUnauthorized() {
        if (!authorized_ || authorized_())
            return;
        authorized_ = {};
        WithdrawPublication();
        phase_ = EditorViewportImagePhase::Cancelled;
    }

    /** @brief Separately revokes transport delivery even if a caller still owns detached pixels. */
    void OpenGLViewportImageCapture::WithdrawPublication() noexcept {
        publication_.reset();
        artifact_.reset();
    }

    /** @brief Queues one bounded color rectangle into an OpenGL pixel pack buffer. */
    bool OpenGLViewportImageCapture::Submit(GLuint framebuffer) {
        const PixelPackState pack;
        const ReadFramebufferState framebufferState(framebuffer);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        glGenBuffers(1, &buffer_);
        if (!buffer_)
            return false;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_);
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(region_.Bytes()), nullptr, GL_STREAM_READ);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        for (std::size_t index = 1; index < PackFields.size(); ++index)
            glPixelStorei(PackFields[index], 0);
        const auto bottom = height_ - region_.y - region_.height;
        glReadPixels(static_cast<GLint>(region_.x), static_cast<GLint>(bottom), static_cast<GLsizei>(region_.width),
                     static_cast<GLsizei>(region_.height), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence_) {
            copyFenceUnavailable_ = true;
            ReleaseCopy();
            return false;
        }
        glFlush();
        return glGetError() == GL_NO_ERROR;
    }

    /** @brief Observes native completion at zero timeout, retaining cancelled staging until its fence signals. */
    void OpenGLViewportImageCapture::Poll() {
        const auto wait = glClientWaitSync(fence_, 0, 0);
        if (wait == GL_TIMEOUT_EXPIRED)
            return;
        if (phase_ == EditorViewportImagePhase::Pending) {
            const bool completed = wait == GL_ALREADY_SIGNALED || wait == GL_CONDITION_SATISFIED;
            phase_ = completed && CopyPixels() ? EditorViewportImagePhase::Ready : EditorViewportImagePhase::Failed;
        }
        ReleaseCopy();
    }

    /** @brief Maps only completed storage, flips rows, and publishes no native identities. */
    bool OpenGLViewportImageCapture::CopyPixels() {
        std::shared_ptr<EditorViewportImageArtifact> result;
        std::shared_ptr<const Publication> publication;
        try {
            result = std::make_shared<EditorViewportImageArtifact>();
            result->region = region_;
            result->rgba.resize(region_.Bytes());
            publication = std::make_shared<const Publication>();
        } catch (const std::bad_alloc &) {
            return false;
        }
        const PixelPackState pack;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer_);
        const auto *pixels = static_cast<const std::byte *>(
            glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(region_.Bytes()), GL_MAP_READ_BIT));
        if (!pixels)
            return false;
        const auto stride = std::size_t{region_.width} * EditorViewportImageRegion::Channels;
        for (std::size_t row = 0; row < region_.height; ++row)
            std::copy_n(pixels + (region_.height - 1 - row) * stride, stride, result->rgba.data() + row * stride);
        if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER) != GL_TRUE)
            return false;
        artifact_ = std::move(result);
        publication_ = std::move(publication);
        return true;
    }

    /** @brief Releases native names without waiting. OpenGL keeps resources pinned by queued commands. */
    void OpenGLViewportImageCapture::ReleaseCopy() noexcept {
        if (fence_)
            glDeleteSync(fence_);
        if (buffer_)
            glDeleteBuffers(1, &buffer_);
        fence_ = nullptr;
        buffer_ = 0;
    }

    /** @copydoc OpenGLViewportImageCapture::Shutdown */
    void OpenGLViewportImageCapture::Shutdown() noexcept {
        ReleaseCopy();
        WithdrawPublication();
        started_.reset();
        authorized_ = {};
        phase_ = EditorViewportImagePhase::Failed;
        copyFenceUnavailable_ = false;
    }
}  // namespace Horo::Editor
