#include "SdlOpenGLPresentationPort.h"

#include "editor/renderer/EditorRendererErrors.h"

#include <glad/gl.h>
#include <limits>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeSdlRenderError(const ErrorCodeDescriptor &descriptor, const char *operation) {
            return MakeError(descriptor, std::string{operation} + ": " + SDL_GetError());
        }

        [[nodiscard]] std::uint16_t ContextVersionComponent(const int value) noexcept {
            constexpr auto maximum = static_cast<int>(std::numeric_limits<std::uint16_t>::max());
            return value > 0 && value <= maximum ? static_cast<std::uint16_t>(value) : 0;
        }
    }  // namespace

    /** @copydoc SdlOpenGLPresentationPort::SdlOpenGLPresentationPort */
    SdlOpenGLPresentationPort::SdlOpenGLPresentationPort(SDL_Window &window) noexcept : window_(&window) {}

    /** @copydoc SdlOpenGLPresentationPort::CreateContext */
    Result<void> SdlOpenGLPresentationPort::CreateContext(const Render::OpenGLContextDescriptor &descriptor) {
        if (context_ != nullptr) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextExists, "An OpenGL context is already retained"));
        }

        if (const int contextFlags = descriptor.enableDebugContext ? SDL_GL_CONTEXT_DEBUG_FLAG : 0;
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, contextFlags) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, descriptor.majorVersion) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, descriptor.minorVersion)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextAttributesFailed, "SDL_GL_SetAttribute failed"));
        }

        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextCreationFailed, "SDL_GL_CreateContext failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::MakeCurrent */
    Result<void> SdlOpenGLPresentationPort::MakeCurrent() {
        if (context_ == nullptr || !SDL_GL_MakeCurrent(window_, context_)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlMakeCurrentFailed, "SDL_GL_MakeCurrent failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::LoadCommandDispatch */
    Result<void> SdlOpenGLPresentationPort::LoadCommandDispatch() {
        if (context_ == nullptr || gladLoadGL(SDL_GL_GetProcAddress) == 0) {
            return Result<void>::Failure(
                MakeSdlRenderError(RendererErrors::ViewportOpenGLDispatchFailed, "OpenGL command dispatch loading failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::QueryContextFacts */
    Result<Render::OpenGLContextFacts> SdlOpenGLPresentationPort::QueryContextFacts() {
        int majorVersion = 0;
        int minorVersion = 0;
        int profile = 0;
        if (context_ == nullptr || !SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &majorVersion) ||
            !SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minorVersion) ||
            !SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile)) {
            return Result<Render::OpenGLContextFacts>::Failure(
                MakeSdlRenderError(RendererErrors::SdlContextAttributesFailed, "OpenGL context inspection failed"));
        }

        GLint maxTextureSize = 0;
        GLint maxColorAttachments = 0;
        GLint maxVertexAttributes = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxColorAttachments);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxVertexAttributes);
        const auto positive = [](const GLint value) {
            return value > 0 ? static_cast<std::uint32_t>(value) : 0U;
        };
        Render::OpenGLContextProfile contextProfile = Render::OpenGLContextProfile::Unknown;
        if ((profile & SDL_GL_CONTEXT_PROFILE_CORE) != 0)
            contextProfile = Render::OpenGLContextProfile::Core;
        else if ((profile & SDL_GL_CONTEXT_PROFILE_COMPATIBILITY) != 0)
            contextProfile = Render::OpenGLContextProfile::Compatibility;

        return Result<Render::OpenGLContextFacts>::Success({
            .apiFamily = (profile & SDL_GL_CONTEXT_PROFILE_ES) != 0 ? Render::OpenGLApiFamily::Embedded : Render::OpenGLApiFamily::Desktop,
            .majorVersion = ContextVersionComponent(majorVersion),
            .minorVersion = ContextVersionComponent(minorVersion),
            .profile = contextProfile,
            .requiredEntryPointsAvailable = GLAD_GL_VERSION_4_1 != 0,
            .maxTexture2DSize = positive(maxTextureSize),
            .maxColorAttachments = positive(maxColorAttachments),
            .maxVertexAttributes = positive(maxVertexAttributes),
        });
    }

    /** @copydoc SdlOpenGLPresentationPort::SetPresentMode */
    Result<void> SdlOpenGLPresentationPort::SetPresentMode(const Render::PresentMode mode) {
        int interval = 0;
        switch (mode) {
            case Render::PresentMode::Fifo:
                interval = 1;
                break;
            case Render::PresentMode::Immediate:
                interval = 0;
                break;
            default:
                return Result<void>::Failure(MakeError(RendererErrors::SdlInvalidPresentMode, "Unsupported presentation mode."));
        }

        if (!SDL_GL_SetSwapInterval(interval)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlPresentModeFailed, "SDL_GL_SetSwapInterval failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::SwapBuffers */
    Result<void> SdlOpenGLPresentationPort::SwapBuffers() {
        if (context_ == nullptr || !SDL_GL_SwapWindow(window_)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlSwapFailed, "SDL_GL_SwapWindow failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::DestroyContext */
    void SdlOpenGLPresentationPort::DestroyContext() noexcept {
        if (context_ != nullptr) {
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }
    }

    /** @copydoc SdlOpenGLPresentationPort::Context */
    SDL_GLContext SdlOpenGLPresentationPort::Context() const noexcept {
        return context_;
    }
}  // namespace Horo::Editor
