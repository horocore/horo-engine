#include "SdlOpenGLPresentationPort.h"

#include "../EditorRendererErrors.h"

#include <glad/gl.h>
#include <limits>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeSdlRenderError(const ErrorCodeDescriptor &descriptor, const char *operation) {
            return MakeError(descriptor, std::string{operation} + ": " + SDL_GetError());
        }

        [[nodiscard]] std::uint16_t ContextVersionComponent(const int value) noexcept {
            constexpr auto maximum = static_cast<int>((std::numeric_limits<std::uint16_t>::max)());
            return value > 0 && value <= maximum ? static_cast<std::uint16_t>(value) : 0;
        }

        /** @brief Applies the requested Core context attributes, preserving SDL's first failed operation. */
        Result<void> ConfigureContextAttributes(const Render::OpenGLContextDescriptor &descriptor) {
            if (const int flags = descriptor.enableDebugContext ? SDL_GL_CONTEXT_DEBUG_FLAG : 0;
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, flags) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, descriptor.majorVersion) ||
                !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, descriptor.minorVersion))
                return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextAttributesFailed, "SDL_GL_SetAttribute failed"));
            return Result<void>::Success();
        }

        /** @brief SDL's native context identity components; never published outside the concrete port. */
        struct NativeContextVersion {
            int major{};
            int minor{};
            int profile{};
        };

        /** @brief Reads attributes only from the retained current context and preserves inspection failure. */
        Result<NativeContextVersion> InspectContextVersion(const SDL_GLContextState *context) {
            NativeContextVersion version;
            if (context == nullptr || SDL_GL_GetCurrentContext() != context ||
                !SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &version.major) ||
                !SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &version.minor) ||
                !SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &version.profile))
                return Result<NativeContextVersion>::Failure(
                    MakeSdlRenderError(RendererErrors::SdlContextAttributesFailed, "OpenGL context inspection failed"));
            return Result<NativeContextVersion>::Success(version);
        }
    }  // namespace

    /** @copydoc SdlOpenGLPresentationPort::SdlOpenGLPresentationPort */
    SdlOpenGLPresentationPort::SdlOpenGLPresentationPort(SDL_Window &window) noexcept : window_(&window) {}

    /** @copydoc SdlOpenGLPresentationPort::CheckOwner */
    Result<void> SdlOpenGLPresentationPort::CheckOwner() const {
        if (std::this_thread::get_id() != ownerThread_)
            return Result<void>::Failure(
                MakeError(RendererErrors::SdlWrongThread, "SDL presentation requires its constructing render owner thread."));
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::CreateContext */
    Result<void> SdlOpenGLPresentationPort::CreateContext(const Render::OpenGLContextDescriptor &descriptor) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (descriptor.profile != Render::OpenGLContextProfile::Core || descriptor.majorVersion == 0)
            return Result<void>::Failure(
                MakeError(RendererErrors::SdlInvalidContextDescriptor,
                          "The OpenGL presentation attachment requires an explicit desktop Core context version."));
        if (context_ != nullptr) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextExists, "An OpenGL context is already retained"));
        }

        if (const auto attributes = ConfigureContextAttributes(descriptor); attributes.HasError())
            return attributes;

        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlContextCreationFailed, "SDL_GL_CreateContext failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::MakeCurrent */
    Result<void> SdlOpenGLPresentationPort::MakeCurrent() {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (context_ == nullptr || !SDL_GL_MakeCurrent(window_, context_)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlMakeCurrentFailed, "SDL_GL_MakeCurrent failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::LoadCommandDispatch */
    Result<void> SdlOpenGLPresentationPort::LoadCommandDispatch() {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (context_ == nullptr || SDL_GL_GetCurrentContext() != context_ || gladLoadGL(SDL_GL_GetProcAddress) == 0) {
            return Result<void>::Failure(
                MakeSdlRenderError(RendererErrors::ViewportOpenGLDispatchFailed, "OpenGL command dispatch loading failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::QueryContextFacts */
    Result<Render::OpenGLContextFacts> SdlOpenGLPresentationPort::QueryContextFacts() {
        if (const auto owner = CheckOwner(); owner.HasError())
            return Result<Render::OpenGLContextFacts>::Failure(owner.ErrorValue());
        const auto version = InspectContextVersion(context_);
        if (version.HasError())
            return Result<Render::OpenGLContextFacts>::Failure(version.ErrorValue());
        const int profile = version.Value().profile;

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
            .majorVersion = ContextVersionComponent(version.Value().major),
            .minorVersion = ContextVersionComponent(version.Value().minor),
            .profile = contextProfile,
            .requiredEntryPointsAvailable = GLAD_GL_VERSION_4_1 != 0,
            .maxTexture2DSize = positive(maxTextureSize),
            .maxColorAttachments = positive(maxColorAttachments),
            .maxVertexAttributes = positive(maxVertexAttributes),
        });
    }

    /** @copydoc SdlOpenGLPresentationPort::SetPresentMode */
    Result<void> SdlOpenGLPresentationPort::SetPresentMode(const Render::PresentMode mode) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
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

        if (context_ == nullptr || SDL_GL_GetCurrentContext() != context_) {
            return Result<void>::Failure(MakeError(RendererErrors::SdlPresentModeFailed, "The retained OpenGL context is not current."));
        }
        return Detail::ApplyExactSwapInterval(interval, SDL_GL_GetSwapInterval, SDL_GL_SetSwapInterval);
    }

    /** @copydoc Detail::SwapIntervalError */
    Result<void> Detail::SwapIntervalError(const SwapIntervalFailure failure) {
        if (failure == SwapIntervalFailure::Query)
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlPresentModeFailed, "SDL_GL_GetSwapInterval failed"));
        if (failure == SwapIntervalFailure::Set)
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlPresentModeFailed, "SDL_GL_SetSwapInterval failed"));
        return Result<void>::Failure(
            MakeError(RendererErrors::SdlPresentModeFailed,
                      "The active native swap interval does not match the explicitly requested presentation mode."));
    }

    /** @copydoc SdlOpenGLPresentationPort::SwapBuffers */
    Result<void> SdlOpenGLPresentationPort::SwapBuffers() {
        if (const auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (context_ == nullptr || SDL_GL_GetCurrentContext() != context_ || !SDL_GL_SwapWindow(window_)) {
            return Result<void>::Failure(MakeSdlRenderError(RendererErrors::SdlSwapFailed, "SDL_GL_SwapWindow failed"));
        }
        return Result<void>::Success();
    }

    /** @copydoc SdlOpenGLPresentationPort::DestroyContext */
    void SdlOpenGLPresentationPort::DestroyContext() noexcept {
        // A foreign caller cannot destroy an owner-thread context; the owner must retry teardown.
        if (std::this_thread::get_id() != ownerThread_)
            return;
        if (context_ != nullptr) {
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
        }
    }

    /** @copydoc SdlOpenGLPresentationPort::Context */
    SDL_GLContext SdlOpenGLPresentationPort::Context() const noexcept {
        return std::this_thread::get_id() == ownerThread_ ? context_ : nullptr;
    }
}  // namespace Horo::Editor
