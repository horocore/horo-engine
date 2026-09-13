#pragma once

/**
 * @file OpenGLBackendModule.h
 * @brief OpenGL backend registration and platform presentation attachment contract.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"

namespace Horo::Render {
    /** @brief OpenGL context profile requested from the platform presentation attachment. */
    enum class OpenGLContextProfile : std::uint8_t {
        Unknown,
        Core,
        Compatibility,
    };

    /** @brief Native-free OpenGL context creation requirements. */
    struct OpenGLContextDescriptor {
        std::uint16_t majorVersion{4};
        std::uint16_t minorVersion{1};
        OpenGLContextProfile profile{OpenGLContextProfile::Core};
        bool enableDebugContext{false};
    };

    /** @brief Actual native context family reported after command dispatch is loaded. */
    enum class OpenGLApiFamily : std::uint8_t {
        Unknown,
        Desktop,
        Embedded,
    };

    /** @brief Native-free facts queried from the current context before backend readiness. */
    struct OpenGLContextFacts {
        OpenGLApiFamily apiFamily{OpenGLApiFamily::Unknown};
        std::uint16_t majorVersion{0};
        std::uint16_t minorVersion{0};
        OpenGLContextProfile profile{OpenGLContextProfile::Unknown};
        bool requiredEntryPointsAvailable{false};
        std::uint32_t maxTexture2DSize{0};
        std::uint32_t maxColorAttachments{0};
        std::uint32_t maxVertexAttributes{0};
    };

    /** @brief OpenGL backend module configuration fixed when its provider is registered. */
    struct OpenGLBackendOptions {
        std::uint16_t majorVersion{4};
        std::uint16_t minorVersion{1};
    };

    /** @brief Returns immutable native-free metadata used before creating the host window. */
    [[nodiscard]] const RenderBackendModuleInfo &GetOpenGLRenderBackendModuleInfo() noexcept;

    /**
     * @brief Platform attachment used by the OpenGL backend to own context and presentation lifecycle.
     *
     * The concrete implementation may use SDL or another window system, but this contract
     * exposes no native type. The port is borrowed: it must outlive the registry provider
     * and every backend/frontend instance created from that provider. All calls and
     * destruction are serialized on the host-declared render-capable thread.
     */
    class IOpenGLPresentationPort {
    public:
        virtual ~IOpenGLPresentationPort() = default;

        /**
         * @brief Creates and internally retains one graphics context for the platform-owned window.
         * @param descriptor Required OpenGL version, profile, and debug policy.
         * @return Success or a typed platform/context creation failure. Failure leaves no retained context.
         */
        [[nodiscard]] virtual Result<void> CreateContext(const OpenGLContextDescriptor &descriptor) = 0;

        /** @brief Makes the retained graphics context current on the calling render-capable thread. */
        [[nodiscard]] virtual Result<void> MakeCurrent() = 0;

        /** @brief Loads the linked OpenGL command dispatch after the retained context becomes current. */
        [[nodiscard]] virtual Result<void> LoadCommandDispatch() = 0;

        /**
         * @brief Queries actual context identity and bounded baseline limits from the current context.
         * @return Owned native-free facts or a typed query failure.
         */
        [[nodiscard]] virtual Result<OpenGLContextFacts> QueryContextFacts() = 0;

        /**
         * @brief Applies backend-neutral presentation pacing to the retained context.
         * @param mode Requested presentation pacing policy.
         * @return Success or a typed unsupported/platform failure.
         */
        [[nodiscard]] virtual Result<void> SetPresentMode(PresentMode mode) = 0;

        /** @brief Presents the current back buffer for the platform-owned window. */
        [[nodiscard]] virtual Result<void> SwapBuffers() = 0;

        /** @brief Destroys the retained context; repeated calls are safe. */
        virtual void DestroyContext() noexcept = 0;
    };

    /**
     * @brief Registers the inert OpenGL backend provider under the canonical `opengl` identity.
     * @param registry Mutable renderer backend registry owned by the host.
     * @param presentationPort Borrowed platform attachment that outlives provider and created backends.
     * @param options OpenGL context version policy captured by the provider.
     * @return Registration result; no context or GPU resource is created by this call.
     */
    [[nodiscard]] Result<void> RegisterOpenGLRenderBackend(RenderBackendRegistry &registry, IOpenGLPresentationPort &presentationPort,
                                                           OpenGLBackendOptions options = {});
}  // namespace Horo::Render
