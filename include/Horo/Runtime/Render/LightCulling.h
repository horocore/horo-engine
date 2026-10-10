#pragma once

/** @file LightCulling.h
 * @brief Bounded deterministic light membership shared by CPU preparation and native compute.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/RenderScene.h"

#include <array>
#include <span>

namespace Horo::Render {
    namespace LightCullingErrors {
        extern const ErrorCodeDescriptor InvalidInput;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Coverage;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor Pending;
    }  // namespace LightCullingErrors

    /** @brief Stable producer identity; sorting never depends on scene slot traversal or GPU scheduling. */
    struct RenderLightIdentity final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const RenderLightIdentity &) const noexcept = default;
    };

    /** @brief Canonical immutable light with an identity valid across this scene's successive frames. */
    struct IdentifiedRenderLight final {
        RenderLightIdentity identity;
        RenderLight light;
    };

    /** @brief Inward world-space half-space; normalized normal and offset use world distance units. */
    struct LightClusterPlane final {
        Math::Vec3 normal;
        float offset{};
        /** @brief Validate finite values and unit-length normal. @return True for an admitted half-space. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Six half-spaces of one conservative cluster volume, ordered by its admitted 3D grid. */
    struct LightCluster final {
        std::array<LightClusterPlane, 6> planes;
        /** @brief Validate every half-space. @return True when all six planes are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Product/cook bounds fixed before a frame; complete coverage never silently degrades. */
    struct LightCullingBudget final {
        static constexpr std::uint32_t HardMaximumLights = 4096;
        static constexpr std::uint32_t HardMaximumClusters = 4096;
        static constexpr std::uint32_t HardMaximumReferences = 1U << 20U;
        std::uint32_t maximumLights{256};
        std::uint32_t maximumClusters{256};
        std::uint32_t referencesPerCluster{64};
        bool requireCompleteCoverage{true};

        /** @brief Validate nonzero bounds and overflow-safe reference capacity. @return True for an admitted envelope. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumLights > 0 && maximumLights <= HardMaximumLights && maximumClusters > 0 &&
                   maximumClusters <= HardMaximumClusters && referencesPerCluster > 0 && referencesPerCluster <= maximumLights &&
                   std::uint64_t{maximumClusters} * referencesPerCluster <= HardMaximumReferences;
        }
    };

    /** @brief Exact per-cluster output range with explicit bounded-degradation evidence. */
    struct LightClusterMembership final {
        std::uint32_t offset{};
        std::uint32_t count{};
        std::uint32_t omitted{};
    };

    /** @brief Exact bounded forward submission count and observable omitted-light evidence. */
    struct ForwardLightSelection final {
        std::uint32_t count{};
        std::uint32_t omitted{};
    };

    /**
     * @brief Conservatively tests whether a directional or punctual light can affect one cluster.
     * @details Point and spot range spheres bound all illuminated points; spot cones may retain false positives.
     * Directional lights affect every cluster. Zero intensity or zero punctual range contributes no light.
     * @param light Valid immutable world-space light.
     * @param cluster Valid six-plane volume.
     * @return Conservative membership without allocation or hidden state.
     * @pre Both arguments are valid.
     */
    [[nodiscard]] bool LightIntersectsCluster(const RenderLight &light, const LightCluster &cluster) noexcept;

    /**
     * @brief Prepares finite CPU cluster lists in caller-owned scratch storage without allocation.
     * @details Synchronous caller-thread work, bounded by maximumLights times maximumClusters.
     * Lights must be strictly ordered by nonzero stable identity. Retention order is stable identity;
     * this is the declared overflow recipe, not an implicit quality fallback. A required-complete
     * overflow fails. Scratch may be modified on failure and must only be published after success.
     * No input or output reference survives this call. The caller quiesces calls before releasing storage.
     * @param lights Canonical immutable table; indices in the output refer to this exact table.
     * @param clusters Immutable admitted grid in canonical cluster order.
     * @param budget Product-selected finite envelope and coverage policy.
     * @param membership Scratch records, at least one per submitted cluster.
     * @param references Scratch indices, at least clusters.size() times referencesPerCluster.
     * @param cancellation Host-owned cooperative cancellation observed between clusters.
     * @return Success or typed invalid, capacity, cancellation or required-coverage failure.
     */
    [[nodiscard]] Result<void> CullLightsCpu(std::span<const IdentifiedRenderLight> lights, std::span<const LightCluster> clusters,
                                             const LightCullingBudget &budget, std::span<LightClusterMembership> membership,
                                             std::span<std::uint32_t> references, const CancellationToken &cancellation = {});

    /**
     * @brief Prepares the established RenderSceneView baseline forward light span using the same canonical membership recipe.
     * @param lights Immutable stable-identity ordered extracted table.
     * @param volume Conservative view or draw volume in world space.
     * @param budget Explicit selected CPU recipe; referencesPerCluster must fit MaximumForwardLights without implicit truncation.
     * @param output Caller-owned RenderLight storage at least referencesPerCluster long; consumed synchronously by RenderSceneView.
     * @param cancellation Cooperative host cancellation.
     * @return Exact selected count and omitted evidence, or original typed failure; output is unchanged on failure.
     */
    [[nodiscard]] Result<ForwardLightSelection> PrepareForwardLights(std::span<const IdentifiedRenderLight> lights,
                                                                     const LightCluster &volume, const LightCullingBudget &budget,
                                                                     std::span<RenderLight> output,
                                                                     const CancellationToken &cancellation = {});
}  // namespace Horo::Render
