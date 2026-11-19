#pragma once

/** @file RenderGraphWorkload.h
 * @brief Typed workloads bound to one immutable compiled render graph.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"
#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/MultiviewGraphWorkload.h"
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/UiPaintWorkload.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "Horo/Runtime/Render/VfxGpuSimulation.h"
#include "Horo/Runtime/Render/VfxGraphWorkload.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace Horo::Render {
    /** @brief Clears or preserves one whole single-mip, single-sample graph color texture, including its admitted layers. */
    struct RenderGraphColorAttachment {
        RenderGraphResourceId texture;
        PrimaryOutputAttachment operations; /**< Clear/Load and Store are required to preserve declared graph contents. */
    };

    /** @brief Clears and preserves one whole depth attachment, including every admitted array layer. */
    struct RenderGraphDepthAttachment {
        RenderGraphResourceId texture;
        float clearDepth{1.0F}; /**< Finite normalized depth in [0,1]; graph use must declare a depth write. */
    };

    /** @brief Copies a finite byte range between two distinct graph buffers. */
    struct RenderGraphBufferCopy {
        RenderGraphResourceId source;
        RenderGraphResourceId destination;
        std::size_t sourceOffset{0};
        std::size_t destinationOffset{0};
        std::size_t byteCount{0};
    };

    /** @brief Copies one tightly packed whole RGBA8 base level from a registered host-visible buffer to a registered texture.
     * @details Source requires CopySource and exactly byteCount bytes. Destination requires CopyDestination and exactly extent,
     * TwoD, one mip, one layer, one sample and the declared format (Rgba8Unorm, R32Float or Rgba32Float).
     * No offsets, row padding, partial uploads, allocation or wait is implied.
     * Graph uses are source Read/CopySource and destination Write/CopyDestination. Submission leases retain both resources;
     * subsequent compiled graph uses establish the final layout. This operation does not implement CreateTexture(initialData).
     */
    struct RenderGraphTextureUpload final {
        RenderGraphResourceId source;
        RenderGraphResourceId destination;
        FramebufferExtent extent;
        std::size_t byteCount{};
        RenderTextureFormat format{RenderTextureFormat::Rgba8Unorm}; /**< Exact tightly packed source and destination pixel format. */
    };

    /** @brief Copies two bounded, caller-selected tiles of the exact stereo R32Float coverage producer into one staging buffer.
     * @details The source is the same submitted graph's owned coverage attachment. Tiles are tightly packed in eye order.
     * Only diagnostic observations are exposed. This copy carries no pixel verdict or compositor capability.
     */
    struct RenderGraphStereoCoverageCopy final {
        static constexpr std::uint32_t MaximumTileAxis = 8;
        static constexpr std::size_t MaximumPixels = 2 * MaximumTileAxis * MaximumTileAxis;
        RenderGraphResourceId source, destination;
        std::array<MultiviewRenderRectangle, 2> tiles;

        /** @brief Returns exactly the packed byte length, or zero for malformed tile dimensions. */
        [[nodiscard]] constexpr std::size_t PackedBytes() const noexcept {
            std::size_t pixels{};
            for (const auto &tile : tiles) {
                if (!tile.width || !tile.height || tile.width > MaximumTileAxis || tile.height > MaximumTileAxis)
                    return 0;
                pixels += static_cast<std::size_t>(tile.width) * tile.height;
            }
            return pixels * sizeof(float);
        }
    };

    /** @brief A device-owned closed stereo coverage reducer, retained through accepted queue execution. */
    class IResidentStereoCoverageReducer {
    public:
        virtual ~IResidentStereoCoverageReducer() = default;
    };

    /** @brief Counters written for one eye by the closed coverage reducer. */
    struct StereoCoverageTally final {
        std::uint32_t visited{}, unresolved{};
    };

    /** @brief Reduces the exact paired motion coverage mask in the producer's graphics submission.
     * @details The mask is sampled read. The exact two eye views belong to the paired coverage raster and are
     * retained by its normal graph lease. The result is a storage read/write over two zero-initialized tallies,
     * preceded by a full copy from the reducer's immutable reset source. No compositor admission is implied.
     */
    struct RenderGraphStereoCoverageReduction final {
        using Tallies = std::array<StereoCoverageTally, 2>;
        static constexpr std::size_t CounterBytes = sizeof(Tallies);
        static_assert(CounterBytes == 16);

        RenderGraphResourceId mask, reset, result;
        std::array<RenderTextureViewHandle, 2> eyes;
        std::array<MultiviewRenderRectangle, 2> rectangles;
        std::shared_ptr<IResidentStereoCoverageReducer> reducer;
    };

    /** @brief One bounded cooked light-culling dispatch over four distinct whole graph buffers.
     * @details The owned preparation lease pins the kernel; graph submission leases pin the buffers.
     * All buffers use Storage semantics: Lights/Clusters read, Membership/References write.
     * Outputs contain exact table indices in canonical order plus explicit omitted counts.
     */
    struct RenderGraphLightCulling {
        RenderGraphResourceId lights;
        RenderGraphResourceId clusters;
        RenderGraphResourceId membership;
        RenderGraphResourceId references;
        LightCullingDispatch dispatch;
        std::uint64_t tableRevision{}; /**< Exact uploaded slot revision; stale graph reuse fails before encoding. */
        std::shared_ptr<IResidentLightCullingKernel> kernel;
    };

    /** @brief One immutable GPU particle candidate over four distinct whole storage buffers.
     * @details Source and forces are read-only; destination and observations are write-only graph uses.
     * Disabled observation still binds a real, distinct four-byte dummy, with no observation copy or map.
     * Graph leases pin buffers and the kernel lease pins the complete compute/draw family. Validation
     * admits metadata and declared dependencies only; it does not acknowledge native submission or
     * establish buffer byte capacities, current registry residency or GPU execution.
     */
    struct RenderGraphVfxGpuCompute final {
        RenderGraphResourceId source, destination, forces, observations;
        VfxGpuSimulationDescriptor descriptor;
        GpuVfxSimulationWork work;
        std::shared_ptr<IResidentVfxGpuKernel> kernel;
        std::shared_ptr<IResidentVfxGpuSimulation> simulationLease;
    };

    /** @brief Direct additive billboard consumer of one renderer-owned GPU particle generation.
     * @details The vertex shader reads the same storage buffer produced by simulation. Fixed-capacity
     * instancing clips inactive lanes; no CPU particle upload, count readback or indirect draw is implied.
     * Color is retained; optional depth is read-only. The frontend and selected native adapter must
     * validate the exact issued simulation/device/generation and retain its resource use through completion.
     */
    struct RenderGraphVfxGpuDraw final {
        RenderGraphResourceId particles, color, depth;
        VfxGpuSimulationId simulation;
        std::uint64_t deviceIncarnation{};
        std::uint64_t generation{};
        VfxGpuSimulationDescriptor descriptor;
        VfxGpuDrawView view;
        PrimaryOutputAttachment colorOperations;
        std::shared_ptr<IResidentVfxGpuKernel> kernel;
        CancellationToken lifetimeCancellation; /**< Revokes new publication without releasing accepted native uses. */
        std::shared_ptr<IResidentVfxGpuSimulation> simulationLease;
    };

    /** @brief Bounded raster subrectangle within the retained target extent. */
    struct RenderMaterialRegion {
        std::uint32_t x{}, y{}, width{}, height{};
    };

    /** @brief One exact mesh/material generation with declared graph vertex and index fetch inputs. */
    struct RenderGraphMaterialDraw final {
        RenderMeshHandle mesh;
        RenderGraphResourceId vertices, indices;
        std::shared_ptr<const ResidentMaterialBinding> material;
    };

    /** @brief One exhaustive graph input mapping; exactly one resident buffer or sampled view is valid. */
    struct RenderGraphMaterialRead final {
        RenderGraphResourceId resource;
        RenderBufferHandle buffer;
        RenderTextureViewHandle texture;
    };

    /** @brief Real bounded graphics material consumer inside the same graph as light compute.
     * @details Spans are synchronous borrows; the frontend completion lease retains all exact meshes, views,
     * materials and graph buffers through native completion. Reads exhaustively map every material uniform,
     * readonly storage buffer and sampled view. Packed implicit parameter allocations are not graph inputs.
     * Native admission verifies exact view parents and mesh buffers against declared graph instances before encoding.
     */
    struct RenderGraphMaterialRaster final {
        static constexpr std::size_t MaximumDraws = 128;
        static constexpr std::size_t MaximumReads = RenderGraphLimits::HardMaxResources;
        /** @brief Bounds four directional shadow tiles, opaque scene, display transform, overlays and output encode. */
        static constexpr std::size_t MaximumRetainedPasses = 8;
        static constexpr std::size_t MaximumRetainedDraws = MaximumDraws * MaximumRetainedPasses;
        RenderGraphResourceId color, depth;
        RenderTextureViewHandle colorView, depthView;
        FramebufferExtent extent;
        PrimaryOutputAttachment colorOperations;
        std::optional<float> depthClear{1.0F};
        std::optional<RenderMaterialRegion> region;
        std::span<const RenderGraphMaterialDraw> draws;
        std::span<const RenderGraphMaterialRead> reads;
        bool depthOnly{};
    };

    /** @brief Exact graph image and separately resolved whole-image view generation. */
    struct RenderGraphReconstructionImage final {
        RenderGraphResourceId resource;
        RenderTextureViewHandle view;
    };

    /** @brief One native reconstruction operation submitted with the canonical graph queue and lease.
     * @details Images are indexed by NativeReconstructionImage. Unused roles are empty. Inputs use Sampled Read,
     * outputs use Storage Write, and every role is on a distinct whole texture. Native preparation resolves the exact
     * pipeline, view generation, image formats and GPU completion ownership.
     */
    struct RenderGraphNativeReconstruction final {
        static constexpr auto ImageCount = static_cast<std::size_t>(NativeReconstructionImage::Count);
        static constexpr auto MaximumPassesPerFrame = MultiviewExtractionLimits::HardMaximumViews;
        NativeReconstructionDispatchPlan dispatch;
        std::array<RenderGraphReconstructionImage, ImageCount> images;
        std::shared_ptr<IResidentNativeReconstructionKernel> kernel;
    };

    /** @brief Exact typed operation; an empty workload represents an ordering-only pass. */
    using RenderGraphWorkload =
        std::variant<std::monostate, PrimaryOutputAttachment, RenderGraphColorAttachment, RenderGraphBufferCopy, RenderGraphLightCulling,
                     RenderGraphVfx, RenderGraphDepthAttachment, RenderGraphMultiview, RenderGraphUiPaint, RenderGraphMaterialRaster,
                     RenderGraphVfxGpuCompute, RenderGraphVfxGpuDraw, RenderGraphExposureGpu, RenderGraphTextureUpload,
                     RenderGraphNativeReconstruction, RenderGraphStereoCoverageCopy, RenderGraphStereoCoverageReduction>;

    /** @brief Validates exact native reconstruction graph roles before resolving backend descriptors.
     * @param request Immutable execution request. @param pass Exact compiled compute pass.
     * @param workload One typed dispatch over graph-local images.
     * @return Success or typed invalid graph. Native ownership and image descriptor validation follow independently.
     */
    [[nodiscard]] Result<void> ValidateNativeReconstructionGraphWorkload(const RenderGraphExecutionRequest &request,
                                                                         const RenderGraphExecutionPass &pass,
                                                                         const RenderGraphNativeReconstruction &workload);

    /** @brief Matches one captured texture against the closed shader role and dispatch extents.
     * @param dispatch Validated packed dispatch. @param image Exact closed image role.
     * @param descriptor Actual resident texture descriptor.
     * @return True for the required sample/storage usage, supported format and whole image shape.
     */
    [[nodiscard]] bool MatchesNativeReconstructionImage(const NativeReconstructionDispatchPlan &dispatch, NativeReconstructionImage image,
                                                        const RenderTextureDescriptor &descriptor) noexcept;

    /** @brief Checks exact whole-upload shape and byte bounds against captured resource descriptors without native mutation.
     * @param upload Explicit tightly packed upload value. @param source Exact source buffer descriptor.
     * @param destination Exact destination texture descriptor.
     * @return Success or typed InvalidGraph; descriptor admission does not establish residency, contents or completion.
     */
    [[nodiscard]] Result<void> ValidateTextureUploadDescriptors(const RenderGraphTextureUpload &upload,
                                                                const RenderBufferDescriptor &source,
                                                                const RenderTextureDescriptor &destination);

    /** @brief Explicit operation for one retained pass in compiled order. */
    struct RenderGraphPassWorkload {
        RenderGraphPassRef pass;
        RenderGraphWorkload workload;
    };

    /**
     * @brief Backend instance resolved by the owning frontend for one exact graph identity.
     *
     * The instance is an opaque token in the selected backend's resource namespace, not a
     * native API handle. Callers cannot fabricate tokens or mix frontend/backend owners.
     * Native validation and retained command references remain the backend's responsibility.
     */
    struct RenderGraphResourceInstance {
        RenderGraphResourceId resource;
        std::uint64_t instance{0};
    };

    /**
     * @brief Frontend-owned submission pins released only by the consuming backend on its owner thread.
     *
     * A successful graph admission borrows this stable lease until the exact native command
     * completes, fails terminally, or is abandoned before submission. Backend shutdown releases
     * every outstanding lease before frontend registry destruction. Release is idempotent.
     */
    class IRenderGraphResourceLease {
    public:
        virtual ~IRenderGraphResourceLease() = default;
        /** @brief Releases registry pins after proven native completion or unsent-frame abandonment. */
        virtual void Release() noexcept = 0;
    };

    /** @brief Explicit completion-lease authority after a backend starts immediate native encoding. */
    enum class RenderGraphLeaseAuthority : std::uint8_t {
        Frontend,
        BackendCompletion,
    };

    /** @brief Synchronous transfer receipt; a backend never retains a pointer to this value. */
    struct RenderGraphLeaseTransfer {
        RenderGraphLeaseAuthority authority{RenderGraphLeaseAuthority::Frontend};
    };

    /**
     * @brief One frontend-authored queue submission over a contiguous compiled pass range.
     * @details Records partition the complete graph in compiled order. The submission's
     * queue must match every covered pass. Wait storage belongs to the caller's compiled
     * plan and remains valid through synchronous admission. An ownership acquire waits
     * on the exact producer submission signal; native backends cannot invent a signal,
     * reorder the records or remap an effective queue. Values remain monotonic across
     * graphs in the same frontend/device lifetime.
     */
    struct RenderGraphQueueSubmission {
        RenderQueueSubmission submission;
        RenderGraphExecutionRange passes;
    };

    /** @brief Finite submission and queue envelope admitted before native command allocation. */
    struct RenderGraphSubmissionLimits {
        static constexpr std::size_t MaximumSubmissions = 128;
        static constexpr std::size_t MaximumQueues = 8;
    };

    /** @brief Explicit frame stage; only the named UI composition admits a second, paint-only graph. */
    enum class RenderGraphExecutionRole : std::uint8_t {
        Scene,
        UiComposition
    };

    /**
     * @brief Synchronously borrowed compiled graph execution request for an active frame.
     *
     * Every retained pass has exactly one workload in compiled order. Every graph resource
     * has one frontend-resolved instance in graph resource order. All validation precedes
     * encoding; failure publishes no partial frame. No CPU wait or implicit queue remapping
     * is permitted. Multi-queue transfers require separately compiled submission timelines.
     */
    struct RenderGraphExecutionRequest {
        FrameToken frame;
        const CompiledRenderGraphExecution &graph;
        std::span<const RenderGraphPassWorkload> workloads;
        std::span<const RenderGraphResourceInstance> resources;
        IRenderGraphResourceLease *lease{nullptr}; /**< Stable frontend-owned lease required for resident graph work. */
        bool transientResourcesAdmitted{
            false}; /**< Frontend validated the exact realized lifetime proof and acquired its completion lease. */
        RenderGraphLeaseTransfer *transfer{
            nullptr}; /**< Immediate backends record retained ownership before any queued command; never borrowed after ExecuteGraph. */
        std::span<const RenderGraphQueueSubmission> submissions;
        /**< Explicit frontend-owned submission authority; empty selects only the transitional single-queue contract. */
        std::span<const RenderGraphMeshInstance> meshes;
        /**< Exact frontend-resolved mesh instances used by typed multiview geometry; never native API handles. */
        std::span<const RenderGraphTextureViewInstance> views;
        RenderGraphExecutionRole role{RenderGraphExecutionRole::Scene};
        std::span<const RenderGraphTargetInstance> targets; /**< Exact prepared targets used by typed native VFX. */
    };

    /** @brief Checks exact GPU particle step metadata, storage uses and non-aliased resolved buffers.
     * @param request Complete borrowed graph and resolved instance table.
     * @param pass Exact retained compute pass in the request.
     * @param workload Immutable source/candidate operation and complete-family preparation lease.
     * @return Success or malformed graph failure, before any native encoding or submission.
     * @details Selected-backend admission must additionally prove native ownership, byte capacities,
     * cooked ABI, current simulation generation and the real submission boundary.
     */
    [[nodiscard]] Result<void> ValidateVfxGpuComputeGraphWorkload(const RenderGraphExecutionRequest &request,
                                                                  const RenderGraphExecutionPass &pass,
                                                                  const RenderGraphVfxGpuCompute &workload);

    /** @brief Checks exact direct GPU particle draw uses, bounded view ABI and generation representation.
     * @param request Complete graph and resolved instance table. @param pass Exact retained graphics pass.
     * @param workload Closed additive billboard operation over one particle buffer and retained attachments.
     * @return Success or malformed graph failure; current residency and native capacities remain native admission obligations.
     */
    [[nodiscard]] Result<void> ValidateVfxGpuDrawGraphWorkload(const RenderGraphExecutionRequest &request,
                                                               const RenderGraphExecutionPass &pass, const RenderGraphVfxGpuDraw &workload);

    /** @brief Validates exact material input/use membership and raster outputs before native resource capture.
     * @param request Complete borrowed frame graph request. @param pass Exact retained graphics pass.
     * @param raster Complete bounded material consumer. @return Typed invalid/unsupported failure or complete admission.
     */
    [[nodiscard]] Result<void> ValidateMaterialRasterWorkload(const RenderGraphExecutionRequest &request,
                                                              const RenderGraphExecutionPass &pass,
                                                              const RenderGraphMaterialRaster &raster);

    /** @brief Checks exact copy-pass uses and graph-local buffer/texture identities for a whole upload.
     * @param request Validated graph request envelope. @param pass Retained copy pass with a valid usage range.
     * @param upload Typed workload for this pass.
     * @return Success or typed InvalidGraph; native descriptor/content checks remain mandatory before encoding.
     */
    [[nodiscard]] Result<void> ValidateTextureUploadGraphWorkload(const RenderGraphExecutionRequest &request,
                                                                  const RenderGraphExecutionPass &pass,
                                                                  const RenderGraphTextureUpload &upload);

    /**
     * @brief Validates a finite graph request before selected-backend native validation.
     * @param request Borrowed graph, exact operation/resource views and completion lease.
     * @return Success or typed malformed, unsupported-workload, queue, or transient-admission failure.
     * @details Checks exact workload/use agreement, bounds, resolved identity coverage and
     * non-overlapping intervals for actual repeated transient instances. Imported storage cannot
     * serve as a transient alias. Native namespaces are compared by resource kind. It
     * creates no native resources and proves no backend instance validity or native synchronization.
     * Explicit submission records additionally prove exhaustive queue-consistent pass partitions,
     * global/per-queue monotonicity and exact ownership/dependency signal-wait edges. Empty
     * records select the transitional single-queue contract and cannot admit ownership transfer.
     * Backends must reject unimplemented submission modes before encoding, without fallback.
     * Unused transient declarations may have no instance. Used transient declarations require
     * the frontend's explicit realized-set admission; every native backend must still validate its own objects.
     */
    [[nodiscard]] Result<void> ValidateRenderGraphExecutionRequest(const RenderGraphExecutionRequest &request);

    /** @brief Validates complete issued forward VFX draws and their exact graph resource roles before native encoding.
     * @param request Immutable graph and frontend-resolved identities. @param pass Retained graphics pass being checked.
     * @param workload Exact prepared VFX snapshot, view order and attachment target.
     * @return Success or typed malformed, stale publication or unsupported recipe failure; performs no native work.
     */
    [[nodiscard]] Result<void> ValidateVfxGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                        const RenderGraphVfx &workload);
}  // namespace Horo::Render
