#pragma once

/** @file PhysicsHeightFieldCook.h
 * @brief Deterministic static heightfield-tile cooking and source-free runtime loading.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Physics {
    /** @brief Qualified bounds for one regular-grid tile; limit failures never truncate a tile. */
    struct PhysicsHeightFieldCookLimits final {
        static constexpr std::uint32_t MaximumDimension = 4'096;
        static constexpr std::uint64_t MaximumSamples = 16'777'216;
        static constexpr std::uint32_t MaximumMaterialSlots = 4'096;
        static constexpr std::uint64_t MaximumPayloadBytes = 256ULL * 1024ULL * 1024ULL;

        std::uint32_t maxDimension{1'024};
        std::uint64_t maxSamples{1'048'576};
        std::uint32_t maxMaterialSlots{256};
        std::uint64_t maxPayloadBytes{MaximumPayloadBytes};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsHeightFieldCookLimits &) const noexcept = default;
    };

    /** @brief Versioned fail-only tile cook settings included in exact cook identity. */
    struct PhysicsHeightFieldCookSettings final {
        static constexpr std::uint32_t CurrentSchemaVersion = 1;
        static constexpr std::uint32_t CurrentAlgorithmVersion = 1;

        std::uint32_t schemaVersion{CurrentSchemaVersion};
        std::uint32_t algorithmVersion{CurrentAlgorithmVersion};
        PhysicsHeightFieldCookLimits limits;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsHeightFieldCookSettings &) const noexcept = default;
    };

    /**
     * @brief One row-major regular-grid source tile in local SI Y-up coordinates.
     *
     * Samples have dimensions width*height. Cells have (width-1)*(height-1) entries.
     * Cell holes are exactly 0 (solid) or 1 (excluded); hole cells use an invalid material
     * identity, while solid cells name a declared non-zero material slot. Declared slots are
     * sorted and unique. Origin, horizontal
     * spacing and vertical sample scale are baked into the artifact before runtime loading.
     * Adjacent tiles retain independent persistent subresources so streaming can replace an
     * exact tile generation at a safe point without invalidating existing cache leases.
     */
    struct PhysicsHeightFieldCookRequest final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId subresource;
        std::uint32_t width{};
        std::uint32_t height{};
        Math::Vec3 origin{};
        float spacingX{};
        float spacingZ{};
        float sampleScaleY{1.0F};
        std::span<const float> samples;
        std::span<const std::uint8_t> cellHoles;
        std::span<const PhysicsMaterialSlotId> cellMaterials;
        std::span<const PhysicsMaterialSlotId> materialSlots;
        PhysicsHeightFieldCookSettings settings;
        PhysicsShapeCookTargetDigest target;
        std::string_view sourceContext;
    };

    /** @brief Complete immutable tile artifact and exact publication descriptor. */
    struct PhysicsHeightFieldCookResult final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        Math::Aabb bounds;
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> payload;
    };

    /** @brief Verified owned tile tables, independent of authoring memory and native solver types. */
    struct LoadedPhysicsHeightField final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        Math::Aabb bounds;
        std::uint32_t width{};
        std::uint32_t height{};
        Math::Vec3 origin{};
        float spacingX{};
        float spacingZ{};
        float sampleScaleY{};
        std::vector<float> samples;
        std::vector<std::uint8_t> cellHoles;
        std::vector<PhysicsMaterialSlotId> cellMaterials;
        std::vector<PhysicsMaterialSlotId> materialSlots;
    };

    /**
     * @brief Validates and deterministically cooks one static heightfield tile offline.
     * @param request Exact normalized source, limits, persistent identity and target.
     * @param cancellation Cooperative cancellation checked during bounded table processing.
     * @return Artifact and descriptor, or a contextual stable validation/capacity/cancellation error.
     * @pre Offline cook only; scene activation and runtime frames must use LoadCookedPhysicsHeightField.
     * @post Failure publishes no partial artifact or cache entry.
     */
    [[nodiscard]] Result<PhysicsHeightFieldCookResult> CookPhysicsHeightField(const PhysicsHeightFieldCookRequest &request,
                                                                              const CancellationToken &cancellation = {});

    /**
     * @brief Verifies an exact tile artifact and constructs owned canonical tables without cooking source.
     * @param descriptor Exact asset/subresource, cache-key, payload and target reference.
     * @param expectedTarget Runtime-captured complete qualified Physics target.
     * @param payload Immutable artifact bytes from the Assets runtime provider.
     * @param limits Runtime count/allocation ceilings, no higher than qualified maxima.
     * @return Owned verified tables or a stable integrity/compatibility/capacity error.
     */
    [[nodiscard]] Result<LoadedPhysicsHeightField> LoadCookedPhysicsHeightField(const PhysicsCookedShapeDescriptor &descriptor,
                                                                                const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                                std::span<const std::uint8_t> payload,
                                                                                const PhysicsHeightFieldCookLimits &limits = {});

    /** @brief Rejects non-static heightfield body motion. @param motion Requested body motion.
     * @return Success for Static or a stable unsupported-motion error. */
    [[nodiscard]] Result<void> ValidatePhysicsHeightFieldMotion(PhysicsMotionType motion);
}  // namespace Horo::Physics
