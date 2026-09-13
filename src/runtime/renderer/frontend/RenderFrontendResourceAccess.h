#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

namespace Horo::Render::Detail {
    /** @brief Internal adapter used by matching integrations without publishing native resource identity. */
    class RenderFrontendResourceAccess final {
    public:
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderBufferHandle buffer);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderMeshHandle mesh);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTextureViewHandle view);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTargetHandle target);
        /** @brief Retains a buffer generation until one accepted queue completion. */
        [[nodiscard]] static Result<void> TrackSubmission(RenderFrontend &frontend, RenderBufferHandle buffer,
                                                          RenderTimelinePoint completion);
        /** @brief Retains a mesh generation until one accepted queue completion. */
        [[nodiscard]] static Result<void> TrackSubmission(RenderFrontend &frontend, RenderMeshHandle mesh, RenderTimelinePoint completion);
        /** @brief Retains a texture generation until one accepted queue completion. */
        [[nodiscard]] static Result<void> TrackSubmission(RenderFrontend &frontend, RenderTextureHandle texture,
                                                          RenderTimelinePoint completion);
        /** @brief Retains a texture-view generation until one accepted queue completion. */
        [[nodiscard]] static Result<void> TrackSubmission(RenderFrontend &frontend, RenderTextureViewHandle view,
                                                          RenderTimelinePoint completion);
        /** @brief Retains a render-target generation until one accepted queue completion. */
        [[nodiscard]] static Result<void> TrackSubmission(RenderFrontend &frontend, RenderTargetHandle target,
                                                          RenderTimelinePoint completion);
        /** @brief Advances one queue and drains bounded completed uses and native retirements. */
        [[nodiscard]] static Result<std::size_t> AcknowledgeCompletion(RenderFrontend &frontend, RenderTimelinePoint completion);
    };
}  // namespace Horo::Render::Detail
