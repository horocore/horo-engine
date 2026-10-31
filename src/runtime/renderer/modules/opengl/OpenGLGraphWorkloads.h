#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "OpenGLBackendInternal.h"

#include <unordered_map>

namespace Horo::Render::Detail {
    struct OpenGLPreparedMaterialState;

    /** @brief Native texture metadata and its preparation-time whole color/depth graph attachment. */
    struct OpenGLGraphTexture {
        RenderTextureDescriptor descriptor;
        bool destroyRequested{false};
        std::uint32_t graphFramebuffer{0};
    };

    /** @brief Synchronously borrowed maps owned by the selected context backend. */
    struct OpenGLGraphResources {
        const std::unordered_map<std::uint32_t, RenderBufferDescriptor> &buffers;
        const std::unordered_map<std::uint32_t, OpenGLGraphTexture> &textures;
        const OpenGLPreparedMaterialState *prepared{}; /**< Optional only for graphs containing no native VFX. */
        const IRenderBackend *owner{};
        std::uint64_t deviceIncarnation{};
        const bool *accepting{}; /**< Borrowed canonical backend initialization state; shutdown is observed between native calls. */
    };

    /**
     * @brief Creates a charged texture's graph attachment before any frame admission.
     * @param functions Current-context native dispatch; callbacks must not throw.
     * @param texture Exact live texture object.
     * @param aspect Exact attachment format role, validated by the owning resource preparation.
     * @return Owned framebuffer name or a typed native failure after cleanup and state restoration.
     */
    [[nodiscard]] Result<std::uint32_t> CreateOpenGLGraphFramebuffer(const OpenGLCommandFunctions &functions, std::uint32_t texture,
                                                                     RenderTextureAspect aspect = RenderTextureAspect::Color);

    /**
     * @brief Validates a complete graph and actual native objects before the first immediate command.
     * @param request Exact graph request and synchronous ownership-transfer receipt.
     * @param resources Current native descriptor maps.
     * @param functions Selected-context dispatch, including optional depth-only clear admission.
     * @return Success or typed identity, range, unsupported, or malformed-workload failure.
     */
    [[nodiscard]] Result<void> ValidateOpenGLGraphWorkloads(const RenderGraphExecutionRequest &request,
                                                            const OpenGLGraphResources &resources, const OpenGLCommandFunctions &functions);

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

    /** @brief Validates issued VFX publications against actual canonical program, VAO, attachment and uniform metadata. */
    [[nodiscard]] Result<void> ValidateOpenGLVfxWorkload(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                                         const RenderGraphVfx &workload);
    /** @brief Executes previously admitted indexed instancing through the canonical material consumer and state guard. */
    [[nodiscard]] Result<void> ExecuteOpenGLVfxWorkload(const RenderGraphExecutionRequest &request, const OpenGLGraphResources &resources,
                                                        const OpenGLCommandFunctions &functions, const RenderGraphVfx &workload);
}  // namespace Horo::Render::Detail
