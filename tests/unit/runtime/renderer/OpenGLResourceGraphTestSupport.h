#pragma once

/** @file OpenGLResourceGraphTestSupport.h
 * @brief Shared deterministic native resource and graph completion evidence for OpenGL regressions.
 */

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendModule.h"

#include <array>
#include <functional>
#include <memory>

namespace Horo::Render::Detail {
    struct OpenGLCommandFunctions;
}

namespace Horo::Render::OpenGLResourceTests {
    class ResourcePresentationPort final : public IOpenGLPresentationPort {
    public:
        bool throwOnMakeCurrent{false};      /**< Owner-scoped exception injection before backend lease transfer. */
        std::function<void()> onMakeCurrent; /**< One-shot synchronous owner reentry; never real context evidence. */

        Result<void> CreateContext(const OpenGLContextDescriptor &) override {
            return Result<void>::Success();
        }

        Result<void> MakeCurrent() override {
            if (throwOnMakeCurrent)
                throw std::bad_alloc{};
            if (onMakeCurrent) {
                auto callback = std::move(onMakeCurrent);
                callback();
            }
            return Result<void>::Success();
        }

        Result<void> LoadCommandDispatch() override {
            return Result<void>::Success();
        }

        Result<OpenGLContextFacts> QueryContextFacts() override {
            return Result<OpenGLContextFacts>::Success({.apiFamily = OpenGLApiFamily::Desktop,
                                                        .majorVersion = 4,
                                                        .minorVersion = 1,
                                                        .profile = OpenGLContextProfile::Core,
                                                        .requiredEntryPointsAvailable = true,
                                                        .maxTexture2DSize = 16384,
                                                        .maxColorAttachments = 8,
                                                        .maxVertexAttributes = 16});
        }

        Result<void> SetPresentMode(PresentMode) override {
            return Result<void>::Success();
        }

        Result<void> SwapBuffers() override {
            return Result<void>::Success();
        }

        void DestroyContext() noexcept override {
            // This test double owns no native context.
        }
    };

    struct ResourceCommandState {
        std::uint32_t nextObject{10};
        int generatedBuffers{0};
        int deletedBuffers{0};
        int generatedVertexArrays{0};
        int deletedVertexArrays{0};
        int generatedTextures{0};
        int deletedTextures{0};
        int generatedFramebuffers{0};
        int deletedFramebuffers{0};
        int uploads{0};
        std::size_t emptyBufferBytes{0};
        int attachments{0};
        std::array<std::uint32_t, 2> attachedTextures{};
        std::array<std::uint32_t, 2> attachmentPoints{};
        bool framebufferComplete{true};
        bool failBufferAllocation{false};
        std::int32_t imageInternalFormat{};
        std::uint32_t imageFormat{};
        std::uint32_t imageType{};
        std::size_t imageBytes{};
    };

    struct GraphCommandState {
        std::uint32_t sourceBinding{81};
        std::uint32_t destinationBinding{82};
        std::array<std::array<std::uint32_t, 2>, 4> copies{};
        std::size_t copyCount{};
        bool failCopy{};
        std::uint32_t depthClears{};
        float depthValue{};
        std::int32_t depthFramebuffer{};
        bool failDepthClear{};
    };

    extern ResourceCommandState resourceCommandState;
    extern GraphCommandState graphCommandState;

    /** @brief Returns the complete deterministic graph/resource dispatch; never creates a real context. */
    [[nodiscard]] Detail::OpenGLCommandFunctions GraphProbeFunctions() noexcept;

    /**
     * @brief Composes the real backend with deterministic native resource and completion dispatch.
     * @param port Borrowed test presentation owner; must outlive the returned frontend.
     * @return Initialized frontend with native graph operations enabled.
     */
    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateGraphFrontend(ResourcePresentationPort &port);
}  // namespace Horo::Render::OpenGLResourceTests
