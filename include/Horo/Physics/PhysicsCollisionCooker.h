#pragma once

/** @file PhysicsCollisionCooker.h
 * @brief Physics-owned normalized collision import and Assets cook contribution.
 */

#include "Horo/Assets/CookCatalog.h"
#include "Horo/Physics/PhysicsConvexHullCook.h"
#include "Horo/Physics/PhysicsHeightFieldCook.h"
#include "Horo/Physics/PhysicsTriangleMeshCook.h"

#include <memory>
#include <variant>

namespace Horo::Physics {
    /** @brief Owned canonical-meter convex source; no borrowed importer buffers survive import. */
    struct PhysicsCollisionConvexSource final {
        std::vector<Math::Vec3> vertices;
        PhysicsConvexHullCookSettings settings;
    };

    /** @brief Owned static mesh source with persistent triangle and material identities. */
    struct PhysicsCollisionMeshSource final {
        std::vector<Math::Vec3> vertices;
        std::vector<PhysicsTriangleMeshSourceTriangle> triangles;
        std::vector<PhysicsMaterialSlotId> materialSlots;
        PhysicsTriangleMeshCookSettings settings;
    };

    /** @brief Owned regular-grid collision tile in normalized SI/Y-up coordinates. */
    struct PhysicsCollisionHeightFieldSource final {
        std::uint32_t width{};
        std::uint32_t height{};
        Math::Vec3 origin{};
        float spacingX{};
        float spacingZ{};
        float sampleScaleY{1.0F};
        std::vector<float> samples;
        std::vector<std::uint8_t> cellHoles;
        std::vector<PhysicsMaterialSlotId> cellMaterials;
        std::vector<PhysicsMaterialSlotId> materialSlots;
        PhysicsHeightFieldCookSettings settings;
    };

    /** @brief One explicitly selected collision subresource; render geometry never implies collision. */
    struct PhysicsCollisionSource final {
        PhysicsShapeSubresourceId subresource;
        std::variant<PhysicsCollisionConvexSource, PhysicsCollisionMeshSource, PhysicsCollisionHeightFieldSource> geometry;
    };

    /**
     * @brief Immutable, host-selected Physics source-format adapter retained by a cooker contribution.
     *
     * Import is synchronous background/tooling work and may be invoked concurrently. Implementations
     * must bound parsing/allocation before constructing the owned normalized result, observe cancellation,
     * normalize axes/units explicitly and never open paths, publish artifacts or retain source views.
     * Foreign formats and repair policies require an explicit adapter; no default guessing is provided.
     */
    class IPhysicsCollisionSourceImporter {
    public:
        virtual ~IPhysicsCollisionSourceImporter() = default;

        /** @brief Returns all byte-affecting importer/policy settings, including normalization and cook defaults.
         * @return Stable version, settings schema and digest; these must remain immutable for the adapter lifetime. */
        [[nodiscard]] virtual Assets::CookerCacheIdentity CacheIdentity() const noexcept = 0;

        /** @brief Imports one bounded source view into owned normalized collision geometry.
         * @param source Assets-owned invocation-scoped bytes and identity.
         * @param cancellation Cooperative parent token.
         * @return Owned collision source, or a stable contextual error. */
        [[nodiscard]] virtual Result<PhysicsCollisionSource> Import(const Assets::CookSourceView &source,
                                                                    const CancellationToken &cancellation) const = 0;
    };

    /** @brief Borrowed verified collision payload; the caller pins the complete Assets payload during use. */
    struct PhysicsCollisionArtifactView final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest; /**< Exact original Assets source bytes, distinct from normalized geometry digest. */
        Sha256Digest configurationDigest;
        std::span<const std::uint8_t> payload;
    };

    /** @brief Verifies a collision publication wrapper and its domain payload without importing or cooking source.
     * @param asset Exact Assets envelope identity.
     * @param bytes Immutable complete logical Assets payload.
     * @param target Complete host-captured Physics target digest.
     * @return Verified descriptor and borrowed inner payload, or a stable artifact/target/limit error.
     * @pre The caller retains bytes while using the returned view. */
    [[nodiscard]] Result<PhysicsCollisionArtifactView> InspectPhysicsCollisionArtifact(const Assets::AssetId &asset,
                                                                                       std::span<const std::uint8_t> bytes,
                                                                                       const PhysicsShapeCookTargetDigest &target);

    /** @brief Creates an inert Physics contribution; Assets remains the sole cache/publication authority.
     * @param type Explicitly registered collision source asset type.
     * @param target Generic Assets target claimed by this contribution.
     * @param physicsTarget Complete qualified Physics cook target; never inferred from a renderer label.
     * @param importer Immutable source adapter retained through all joined cook jobs.
     * @return Contribution for explicit catalog registration, or DescriptorInvalid for missing adapter/identity.
     * @note Compound and analytic shapes are outside this asset-derived contribution. No runtime cooker is registered. */
    [[nodiscard]] Result<Assets::CookerContribution> MakePhysicsCollisionCookerContribution(
        const Assets::AssetTypeId &type, AssetCookTargetId target, const PhysicsShapeCookTargetDigest &physicsTarget,
        std::shared_ptr<const IPhysicsCollisionSourceImporter> importer);
}  // namespace Horo::Physics
