#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "OpenGLBackendInternal.h"

#include <unordered_map>

namespace Horo::Render::Detail {
    /** @brief Native texture metadata and its preparation-time whole-color graph attachment. */
    struct OpenGLGraphTexture {
        RenderTextureDescriptor descriptor;
        bool destroyRequested{false};
        std::uint32_t graphFramebuffer{0};
    };

    /** @brief Synchronously borrowed maps owned by the selected context backend. */
    struct OpenGLGraphResources {
        const std::unordered_map<std::uint32_t, RenderBufferDescriptor> &buffers;
        const std::unordered_map<std::uint32_t, OpenGLGraphTexture> &textures;
    };

    /**
     * @brief Creates a charged texture's graph attachment before any frame admission.
     * @param functions Current-context native dispatch; callbacks must not throw.
     * @param texture Exact live color texture object.
     * @return Owned framebuffer name or a typed native failure after cleanup and state restoration.
     */
    [[nodiscard]] Result<std::uint32_t> CreateOpenGLGraphFramebuffer(const OpenGLCommandFunctions &functions, std::uint32_t texture);

    /**
     * @brief Validates a complete graph and actual native objects before the first immediate command.
     * @param request Exact graph request and synchronous ownership-transfer receipt.
     * @param resources Current native descriptor maps.
     * @return Success or typed identity, range, unsupported, or malformed-workload failure.
     */
    [[nodiscard]] Result<void> ValidateOpenGLGraphWorkloads(const RenderGraphExecutionRequest &request,
                                                            const OpenGLGraphResources &resources);

    /**
     * @brief Encodes admitted serial clear/load and copy workloads without allocation or waits.
     * @param request Complete previously validated graph request.
     * @param resources Stable native metadata borrowed for this call.
     * @param functions Current-context non-throwing native dispatch.
     * @param outputExtent Admitted primary output extent.
     * @return Success or a typed command failure; the backend completion slot already owns the lease.
     */
    [[nodiscard]] Result<void> ExecuteOpenGLGraphWorkloads(const RenderGraphExecutionRequest &request,
                                                           const OpenGLGraphResources &resources, const OpenGLCommandFunctions &functions,
                                                           FramebufferExtent outputExtent);
}  // namespace Horo::Render::Detail
