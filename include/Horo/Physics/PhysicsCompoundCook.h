#pragma once

/** @file PhysicsCompoundCook.h
 * @brief Bounded flat convex compound artifacts in a shared baked local coordinate space.
 */

#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"

namespace Horo::Physics {
    /** @brief Qualified flat compound limits; nesting and non-convex leaves are explicitly unsupported. */
    struct PhysicsCompoundCookLimits final {
        static constexpr std::uint32_t MaximumChildren = 256;
        std::uint32_t maximumChildren{MaximumChildren};
        std::uint64_t maximumPayloadBytes{PhysicsConvexHullCookLimits::MaximumPayloadBytes};
        PhysicsConvexHullCookLimits convex;
    };

    /** @brief Borrowed offline child bytes with persistent identity and explicit collision material slot. */
    struct PhysicsCompoundCookChild final {
        PhysicsShapeSubresourceId id;
        PhysicsMaterialSlotId material;
        PhysicsCookedShapeDescriptor descriptor;
        std::span<const std::uint8_t> payload;
    };

    /** @brief Captured immutable compound input; child transforms/scales are already baked into the leaf geometry. */
    struct PhysicsCompoundCookRequest final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId subresource;
        PhysicsShapeCookTargetDigest target;
        Sha256Digest dependencyDigest; /**< Exact upstream semantic revision/settings fingerprint. */
        std::span<const PhysicsCompoundCookChild> children;
        PhysicsCompoundCookLimits limits;
    };

    /** @brief Complete detached target-specific artifact for Assets storage and package publication. */
    struct PhysicsCompoundCookResult final {
        PhysicsCookedShapeDescriptor descriptor;
        std::vector<std::uint8_t> payload;
    };

    /** @brief Verified immutable runtime child; stable IDs are never native subshape indexes. */
    struct LoadedPhysicsCompoundChild final {
        PhysicsShapeSubresourceId id;
        PhysicsMaterialSlotId material;
        LoadedPhysicsConvexHull hull;
    };

    /** @brief Owned verified runtime product, requiring neither source geometry nor a cook service. */
    struct LoadedPhysicsCompound final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest dependencyDigest;
        std::vector<LoadedPhysicsCompoundChild> children;
    };

    /**
     * @brief Validates convex leaves and encodes a flat compound in stable child-ID order.
     * @param request Exact identity, target, dependency and borrowed immutable child payloads.
     * @param cancellation Cooperative cancellation observed between bounded children and before completion.
     * @return Complete detached artifact or typed failure; no partial publication or capability fallback.
     * @pre Offline/control use only. Caller retains input bytes until this synchronous call completes.
     */
    [[nodiscard]] Result<PhysicsCompoundCookResult> CookPhysicsCompound(const PhysicsCompoundCookRequest &request,
                                                                        const CancellationToken &cancellation = {});

    /**
     * @brief Verifies and decodes packaged compound bytes through the qualified convex runtime loader.
     * @param descriptor Exact Assets catalog reference.
     * @param expectedTarget Complete runtime Physics target.
     * @param payload Immutable packaged bytes.
     * @param limits Finite runtime allocation limits, at most the qualified maxima.
     * @return Owned canonical convex children or typed integrity/target/capacity failure.
     */
    [[nodiscard]] Result<LoadedPhysicsCompound> LoadCookedPhysicsCompound(const PhysicsCookedShapeDescriptor &descriptor,
                                                                          const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                          std::span<const std::uint8_t> payload,
                                                                          const PhysicsCompoundCookLimits &limits = {});
}  // namespace Horo::Physics
