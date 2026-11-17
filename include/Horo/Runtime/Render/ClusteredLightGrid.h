#pragma once
/** @file ClusteredLightGrid.h @brief Camera-defined 3D light grids and bounded prepared lists over the canonical light table. */
#include "Horo/Runtime/Render/LightCulling.h"
#include "Horo/Runtime/Render/TemporalHistory.h"

#include <optional>

namespace Horo::Render {
    /** @brief Exact producer/view domain; generations describe actual immutable content/configuration changes, never frame ordinals. */
    struct ClusteredLightIdentity final {
        RenderViewId view;
        std::uint64_t sceneDomain{}, lightTableGeneration{}, projectionGeneration{}, gridGeneration{};
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const ClusteredLightIdentity &) const noexcept = default;
    };
    enum class LightClusterDepthPartition : std::uint8_t {
        Linear,
        Logarithmic
    };

    /** @brief Complete camera-owned grid definition; x/y/z are counts, ordered x + X*(y + Y*z). */
    struct ClusteredLightGridDescriptor final {
        ClusteredLightIdentity identity;
        RenderCameraView camera;
        FramebufferExtent extent;
        std::uint32_t x{}, y{}, z{};
        LightClusterDepthPartition depthPartition{LightClusterDepthPartition::Logarithmic};
        Math::ClipDepthRange depthRange{Math::ClipDepthRange::ZeroToOne};
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Owned fixed-size metadata; separate caller-owned cluster storage contains world-space planes for the existing packed ABI. */
    struct ClusteredLightGrid final {
        ClusteredLightGridDescriptor descriptor;
        Math::Mat4 viewProjection{Math::Mat4::Identity()};
        std::uint32_t clusterCount{};
        std::uint64_t clusterSignature{}; /**< Exact prepared plane signature, checked before list preparation. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Builds genuine 3D camera-frustum cells without heap allocation on success.
     * @param descriptor Exact camera, domain, projection, extent, depth convention and finite grid dimensions.
     * @param budget Product-selected existing maximum4096 light/cluster/reference envelope.
     * @param clusters Caller-owned staging storage for every cell, never retained. Failure may alter unpublished staging.
     * @param cancellation Cooperative caller cancellation checked between cells.
     * @return Owned metadata or typed invalid/capacity/math/cancellation failure. Publish metadata and scratch together only on success.
     */
    [[nodiscard]] Result<ClusteredLightGrid> BuildClusteredLightGrid(const ClusteredLightGridDescriptor &descriptor,
                                                                     const LightCullingBudget &budget, std::span<LightCluster> clusters,
                                                                     const CancellationToken &cancellation = {});
    /** @brief Maps a top-left-origin pixel and supplied normalized device depth to a 3D cell.
     * @details Interprets the supplied quantized depth literally. Projection/depth-buffer rounding can
     * lose geometric view depth, especially at large far/near ratios; this query cannot recover that
     * information. Raster shading with actual geometry must use MapClusteredLightViewDepth instead.
     * @param grid Successful immutable grid metadata.
     * @param pixelX Pixel coordinate in [0,width); native bottom-left origins must be converted by their owner.
     * @param pixelY Pixel coordinate in [0,height).
     * @param deviceDepth Window depth in [0,1] for either clip convention, not signed OpenGL NDC or world distance.
     * @return Linear cluster index, empty for finite out-of-view coordinates/depth, or typed invalid input.
     */
    [[nodiscard]] Result<std::optional<std::uint32_t>> MapClusteredLightPixel(const ClusteredLightGrid &grid, float pixelX, float pixelY,
                                                                              float deviceDepth);

    /** @brief Maps a top-left pixel and actual positive camera-forward distance to the geometric 3D cell.
     * @details Uses the same depth partition as MapClusteredLightPixel without reconstructing distance
     * from quantized device depth. The raster consumer obtains distance as dot(worldPosition-camera.position,
     * normalize(camera.target-camera.position)); perspective and orthographic cameras share this definition.
     * @param grid Successful immutable grid metadata for that exact camera and viewport.
     * @param pixelX Pixel coordinate in [0,width), with native origins converted by the owner.
     * @param pixelY Top-left-origin pixel coordinate in [0,height).
     * @param viewDepth Finite positive camera-forward distance in world meters, not radial distance or window depth.
     * @return Linear cluster index, empty outside viewport/near/far bounds, or typed invalid input for
     * invalid metadata, nonfinite coordinates or nonpositive distance. No borrows are retained or allocation performed.
     */
    [[nodiscard]] Result<std::optional<std::uint32_t>> MapClusteredLightViewDepth(const ClusteredLightGrid &grid, float pixelX,
                                                                                  float pixelY, float viewDepth);

    /** @brief Aggregate exact CPU-reference evidence; no diagnostic payload or per-frame allocation. */
    struct ClusteredLightDiagnostics final {
        std::uint32_t lightCount{}, clusterCount{}, overflowedClusterCount{}, maximumRetainedPerCluster{};
        std::uint64_t retainedReferenceCount{}, omittedReferenceCount{};
        bool completeCoverage{true};
    };

    /** @brief Prepared immutable synchronous borrows. All backing tables/scratch must outlive every consumer and remain unchanged.
     * @details Preparation owns no jobs or GPU objects. The host stages separate storage, publishes only success at its
     * render safe point, and quiesces readers before scratch reuse or shutdown. This does not claim native compute execution.
     */
    struct PreparedClusteredLights final {
        ClusteredLightGrid grid;
        LightCullingBudget budget;
        std::span<const IdentifiedRenderLight> lights;
        std::span<const LightCluster> clusters;
        std::span<const LightClusterMembership> membership;
        std::span<const std::uint32_t> references;
        ClusteredLightDiagnostics diagnostics;
        [[nodiscard]] Result<std::span<const std::uint32_t>> ReferencesForCluster(std::uint32_t index) const;
    };

    /** @brief Prepares complete bounded lists using the existing deterministic CPU reference and explicit overflow policy.
     * @param grid Exact successful metadata paired with clusters.
     * @param lights Full strictly stable-ID-ordered canonical table in grid.descriptor.identity's scene/content domain.
     * @param clusters Immutable world-plane prefix from the corresponding successful grid preparation.
     * @param budget Existing product budget; requireCompleteCoverage fails instead of publishing omissions.
     * @param membership Caller-owned unpublished output storage, at least clusterCount records.
     * @param references Caller-owned unpublished output storage, at least clusterCount*referencesPerCluster indices.
     * @param cancellation Cooperative cancellation; no asynchronous lifetime survives return.
     * @return Metadata and immutable borrows after success, or original typed failure. Failed scratch must not replace prior ready lists.
     */
    [[nodiscard]] Result<PreparedClusteredLights> PrepareClusteredLightLists(
        const ClusteredLightGrid &grid, std::span<const IdentifiedRenderLight> lights, std::span<const LightCluster> clusters,
        const LightCullingBudget &budget, std::span<LightClusterMembership> membership, std::span<std::uint32_t> references,
        const CancellationToken &cancellation = {});

    /** @brief Exact per-draw CPU fallback result; source indices retain global shadow/light identity after local selection. */
    struct ForwardDrawLightSelection final {
        std::uint32_t count{}, omitted{};
        std::array<std::uint32_t, MaximumForwardLights> sourceIndices{};
    };

    /** @brief Culls the full canonical table against one actual world-space draw AABB without global truncation.
     * @param lights Complete canonical stable-ID-ordered table, up to its admitted maximum4096 entries.
     * @param worldBounds Conservative actual transformed draw bounds.
     * @param budget Explicit BaselineForward CPU budget; referencesPerCluster must fit the actual sixteen-light draw ABI.
     * @param output Caller-owned draw-light staging prefix; unchanged on any failure.
     * @param cancellation Cooperative caller cancellation.
     * @return Exact local count, omitted evidence and source-table indices, or typed unsupported/coverage/capacity/invalid failure.
     */
    [[nodiscard]] Result<ForwardDrawLightSelection> PrepareDrawForwardLights(std::span<const IdentifiedRenderLight> lights,
                                                                             const Math::Aabb &worldBounds,
                                                                             const LightCullingBudget &budget,
                                                                             std::span<RenderLight> output,
                                                                             const CancellationToken &cancellation = {});

    enum class LightPreparationMode : std::uint8_t {
        BaselineForwardCpu,
        ClusteredForwardNative
    };
    enum class LightPreparationFallback : std::uint8_t {
        Forbidden,
        AllowBaselineForwardCpu
    };
    enum class LightPreparationMissing : std::uint8_t {
        None = 0,
        NativeCulling = 1,
        StorageRaster = 2,
        SameGraphConsumer = 4,
        DepthPrepass = 8
    };

    /** @brief Actual selected implementation predicates, distinct from generic Compute or backend-name inference. */
    struct LightPreparationSupport final {
        bool nativeCulling{}, storageRaster{}, sameGraphConsumer{}, depthPrepass{};
    };

    struct LightPreparationSelection final {
        LightPreparationMode mode{LightPreparationMode::BaselineForwardCpu};
        std::uint8_t missingPredicates{}; /**< Bitwise LightPreparationMissing evidence for an explicitly selected CPU fallback. */
    };

    /** @brief Selects only an explicitly permitted complete recipe; never advertises compute for CPU work.
     * @param requested Authored requested family.
     * @param fallback Authored permission for BaselineForward CPU when native clustered predicates fail.
     * @param support Actual native LightCulling operation, storage raster, same-graph consumer and depth-prepass availability.
     * @return Exact selected family and missing-predicate evidence, or typed invalid/unsupported failure.
     */
    [[nodiscard]] Result<LightPreparationSelection> SelectLightPreparation(LightPreparationMode requested,
                                                                           LightPreparationFallback fallback,
                                                                           const LightPreparationSupport &support);
}  // namespace Horo::Render
