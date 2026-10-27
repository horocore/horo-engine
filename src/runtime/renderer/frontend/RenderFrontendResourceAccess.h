#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

namespace Horo::Render::Detail {
    /** @brief Internal adapter used by matching integrations without publishing native resource identity. */
    class RenderFrontendResourceAccess final {
    public:
        /** @brief Cold-admits owned depth backing for an explicitly supported complete multiview workload.
         * @param frontend Selected frontend whose owner thread must be open and outside a frame.
         * @param scope Canonical memory scope charged for this resource.
         * @param extent Exact single-sample depth extent.
         * @param viewCount Complete view count, bounded by the selected backend's effective multiview support.
         * @return Pending Depth32Float attachment storage with one layer per view, or the original admission/budget error.
         * @details Uses the ordinary registry, upload queue, generation and retirement owners. It admits no initial data,
         * sampled usage, arbitrary array format or native handle. Ordinary CreateTexture admission is unchanged.
         */
        [[nodiscard]] static Result<ResourceCreation<RenderTextureHandle>> CreateMultiviewDepth(RenderFrontend &frontend,
                                                                                                RenderMemoryScopeId scope,
                                                                                                FramebufferExtent extent,
                                                                                                std::uint32_t viewCount);
        /** @brief Retires an owned creation, recognizing only its already-retired failed operation as complete.
         * @param frontend Same canonical creation owner.
         * @param creation Unmodified handle/operation pair returned by this frontend's creation request.
         * @return Ordinary release result, or success when that exact failed request was already rolled back.
         * @details Live generations, pending retirement and unknown/evicted journals retain their original errors.
         */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend, const ResourceCreation<RenderTextureHandle> &creation);
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend,
                                                         const ResourceCreation<RenderTextureViewHandle> &creation);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderBufferHandle buffer);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderMeshHandle mesh);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTextureViewHandle view);
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderTargetHandle target);
        /** @brief Borrows an exact ready pipeline for selected-backend integrations and native qualification only. */
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderPipelineHandle pipeline);
        /** @brief Borrows an exact ready sampler for selected-backend integrations and native qualification only. */
        [[nodiscard]] static Result<std::uint64_t> BackendInstance(const RenderFrontend &frontend, RenderSamplerHandle sampler);
        /** @brief Queries native block storage through the selected backend on the cold preparation owner thread.
         * @param frontend Selected frontend whose pipeline generation must remain ready.
         * @param descriptor Pipeline and exact reflected logical layout; resource values are not required for this query.
         * @param binding Uniform block whose native storage extent is required.
         * @return Backend-owned padded extent, or an admission, stale identity or native layout error.
         */
        [[nodiscard]] static Result<std::size_t> MaterialParameterBufferBytes(const RenderFrontend &frontend,
                                                                              const MaterialBindingDescriptor &descriptor,
                                                                              ShaderBindingId binding);
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
