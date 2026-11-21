#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <functional>

namespace Horo::Render {
    struct UiPaintProgramDescriptor;
    struct UiPaintSceneCanvasSource;
    class UiPaintPreparedFrame;
    struct CookedUiPaintKernel;
}  // namespace Horo::Render

namespace Horo::Runtime::Ui {
    class UiRenderSnapshot;
}

namespace Horo::Render::Detail {
    struct RenderGraphExternalTextureSlot;
    struct RenderGraphExternalTextureBinding;
    struct RenderGraphOptionalRead;

    /** @brief Cold authoring of a sampled canvas scene, including its optionally acquired attachments. */
    struct UiPaintSceneCanvasRecipe final {
        RenderGraphLimits limits;
        RenderQueueId graphicsQueue;
        std::function<Result<RenderGraphPassRef>(RenderGraphBuilder &, const UiPaintSceneCanvasSource &,
                                                 std::vector<RenderGraphImportedState> &, std::vector<RenderGraphExternalTextureSlot> &,
                                                 std::vector<RenderGraphOptionalRead> &)>
            author;
    };

    /** @brief Internal adapter used by matching integrations without publishing native resource identity. */
    class RenderFrontendResourceAccess final {
    public:
        /** @brief Prepares the writer and its consumer with one graph per resident paint slot.
         * @param frontend Live renderer owner. @param descriptor Paint projection and capacities.
         * @param prototype Canonical source roles. @param textures Current sampled generations.
         * @param kernel Cooked paint interface. @param scene Cold scene recipe, including any external slots.
         * @param cancellation Preparation fence. @return Prepared owner or the original preparation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<UiPaintProgram>> CreateSceneCanvas(
            RenderFrontend &frontend, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
            std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const UiPaintSceneCanvasRecipe &scene,
            const CancellationToken &cancellation = {});
        /** @brief Binds the current acquired inputs before the frame. The ordinary graph lease consumes the binding on execution.
         * @param program Prepared owner. @param slot Retired paint slot for the upcoming frame.
         * @param bindings Complete external acquisitions in declaration order. @param reads Exact subset of optional buffer reads.
         * @return Borrowed bound execution for inspecting pass order until execution or rebind, or a typed failure.
         * @post Paint must be prepared after each bind. The binding cannot be used for a second execution.
         */
        [[nodiscard]] static Result<const CompiledRenderGraphExecution *> BindExternalSceneCanvas(
            UiPaintProgram &program, std::uint32_t slot, std::span<const RenderGraphExternalTextureBinding> bindings,
            std::span<const RenderGraphOptionalRead> reads);
        /** @brief Records a sampled scene and returns the exact signal reserved for its single graph submission.
         * @param program Prepared owner. @param frame Acquired renderer frame. @param prepared Current slot publication.
         * @param source Matching canvas geometry and textures. @param sceneWorkloads Cold-declared scene passes in compiled order.
         * @return Signal of recorded work or a typed failure. The host still verifies queue acceptance and completion.
         */
        [[nodiscard]] static Result<RenderTimelinePoint> RecordSubmittedSceneCanvas(
            UiPaintProgram &program, RenderFrameScope &frame, const UiPaintPreparedFrame &prepared, UiRenderSubmission source,
            std::span<const RenderGraphPassWorkload> sceneWorkloads);
        /** @brief Checks whether a live SceneCanvas program can admit the same paint roles and resource generations without rebuilding. */
        [[nodiscard]] static bool MatchesSceneCanvas(const UiPaintProgram &program, const UiPaintProgramDescriptor &policy,
                                                     const Runtime::Ui::UiRenderSnapshot &prototype,
                                                     std::span<const RenderTextureHandle> textures) noexcept;
        /** @brief Validates the existing open graphics owner outside any active frame.
         * @param frontend Selected existing frontend. @return Exact registry incarnation or original lifecycle error. */
        [[nodiscard]] static Result<RenderResourceOwnerId> RequirePreparationBoundary(const RenderFrontend &frontend);
        /** @brief Borrows the frontend's actual issued queue topology at a cold preparation boundary.
         * @return Existing immutable assignments, valid until frontend teardown; never synthesized native queue ordinals. */
        [[nodiscard]] static Result<std::span<const RenderQueueAssignment>> QueueAssignments(const RenderFrontend &frontend);
        /** @brief Copies one exact completed native observation directly into the existing queue's writable byte arena.
         * @param frontend Same live render owner outside an active frame. @param preparation Exact prepared simulation.
         * @param step Actual completed observe=true step. @param destination Queue-owned writable result storage.
         * @return Actual copied bytes or original native error; consumes native hold only on complete successful copy.
         */
        [[nodiscard]] static Result<std::size_t> CopyCompletedVfxGpuObservation(RenderFrontend &frontend,
                                                                                const VfxGpuPreparationReceipt &preparation,
                                                                                const VfxGpuStepIdentity &step,
                                                                                std::span<std::byte> destination);
        /** @brief Releases a completed native observation without a CPU copy; pending/uncertain work remains retained.
         * @return Successful exact discard or original typed native pending/error.
         */
        [[nodiscard]] static Result<void> DiscardCompletedVfxGpuObservation(RenderFrontend &frontend,
                                                                            const VfxGpuPreparationReceipt &preparation,
                                                                            const VfxGpuStepIdentity &step);
        /** @brief Cold-admits owned depth backing for an explicitly supported complete multiview workload.
         * @param frontend Selected frontend whose owner thread must be open and outside a frame.
         * @param scope Canonical memory scope charged for this resource.
         * @param extent Exact single-sample depth extent.
         * @param viewCount Complete view count, bounded by the selected backend's effective multiview support.
         * @param sampled Also admits sampled usage for explicitly retained previous depth.
         * @return Pending Depth32Float
         * attachment storage with one layer per view, or the original admission/budget error.
         * @details Uses the ordinary registry, upload queue, generation and retirement owners. It admits no initial data,
         * arbitrary array format or native handle. Ordinary CreateTexture admission is unchanged.
         */
        [[nodiscard]] static Result<ResourceCreation<RenderTextureHandle>> CreateMultiviewDepth(
            RenderFrontend &frontend, RenderMemoryScopeId scope, FramebufferExtent extent, std::uint32_t viewCount, bool sampled = false);
        /** @brief Cold-admits only the bounded host-visible counter of the selected stereo reduction route.
         * @param frontend Selected frontend on its owner thread, after exact native route admission.
         * @param scope Canonical memory scope charged for the counter.
         * @return Pending counter storage with no initialized contents or the original admission/budget error.
         */
        [[nodiscard]] static Result<ResourceCreation<RenderBufferHandle>> CreateStereoCoverageCounter(RenderFrontend &frontend,
                                                                                                      RenderMemoryScopeId scope);
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
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend, const ResourceCreation<RenderBufferHandle> &creation);
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend, const ResourceCreation<RenderMeshHandle> &creation);
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend,
                                                         const ResourceCreation<RenderShaderModuleHandle> &creation);
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend, const ResourceCreation<RenderPipelineHandle> &creation);
        /** @copydoc RetireCreation(RenderFrontend &, const ResourceCreation<RenderTextureHandle> &) */
        [[nodiscard]] static Result<void> RetireCreation(RenderFrontend &frontend, const ResourceCreation<RenderSamplerHandle> &creation);
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
