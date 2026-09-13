#pragma once

/**
 * @file PhysicsTriangleMeshCook.h
 * @brief Deterministic static triangle-mesh cooking and source-free runtime loading.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsBodyDescriptor.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Physics {
    /** @brief Explicit over-limit policy; the baseline never truncates collision topology or material mappings. */
    enum class PhysicsTriangleMeshLimitPolicy : std::uint8_t {
        Fail,
        Count,
    };

    /** @brief Qualified count and payload limits for one static triangle-mesh cook. */
    struct PhysicsTriangleMeshCookLimits final {
        static constexpr std::uint32_t MaximumSourceVertices = 1'000'000;
        static constexpr std::uint32_t MaximumTriangles = 2'000'000;
        static constexpr std::uint32_t MaximumMaterialSlots = 4'096;
        static constexpr std::uint64_t MaximumPayloadBytes = 256ULL * 1024ULL * 1024ULL;

        std::uint32_t maxSourceVertices{65'536};
        std::uint32_t maxTriangles{131'072};
        std::uint32_t maxMaterialSlots{256};
        std::uint64_t maxPayloadBytes{MaximumPayloadBytes};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsTriangleMeshCookLimits &) const noexcept = default;
    };

    /** @brief Versioned static triangle-mesh settings that participate in cook identity. */
    struct PhysicsTriangleMeshCookSettings final {
        static constexpr std::uint32_t CurrentSchemaVersion = 1;
        static constexpr std::uint32_t CurrentAlgorithmVersion = 1;

        std::uint32_t schemaVersion{CurrentSchemaVersion};
        std::uint32_t algorithmVersion{CurrentAlgorithmVersion};
        PhysicsTriangleMeshLimitPolicy limitPolicy{PhysicsTriangleMeshLimitPolicy::Fail};
        PhysicsTriangleMeshCookLimits limits;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsTriangleMeshCookSettings &) const noexcept = default;
    };

    /** @brief One normalized source triangle with persistent query/contact and physical-material identity. */
    struct PhysicsTriangleMeshSourceTriangle final {
        std::array<std::uint32_t, 3> vertexIndices{};
        PhysicsShapeSubresourceId subshape;
        PhysicsMaterialSlotId materialSlot;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsTriangleMeshSourceTriangle &) const noexcept = default;
    };

    /** @brief Immutable normalized-meter input captured by the offline Physics shape cooker. */
    struct PhysicsTriangleMeshCookRequest final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId subresource;
        std::span<const Math::Vec3> vertices;
        std::span<const PhysicsTriangleMeshSourceTriangle> triangles;
        std::span<const PhysicsMaterialSlotId> materialSlots;
        PhysicsTriangleMeshCookSettings settings;
        PhysicsShapeCookTargetDigest target;
        std::string_view sourceContext;
    };

    /** @brief Complete deterministic static triangle-mesh cook output ready for Assets publication. */
    struct PhysicsTriangleMeshCookResult final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        Math::Aabb bounds;
        std::uint32_t vertexCount{};
        std::uint32_t triangleCount{};
        std::uint32_t materialSlotCount{};
        std::vector<std::uint8_t> payload;
    };

    /** @brief One verified cooked triangle retaining stable subshape and material-slot identity. */
    struct LoadedPhysicsTriangle final {
        std::array<std::uint32_t, 3> vertexIndices{};
        PhysicsShapeSubresourceId subshape;
        PhysicsMaterialSlotId materialSlot;

        [[nodiscard]] constexpr auto operator<=>(const LoadedPhysicsTriangle &) const noexcept = default;
    };

    /** @brief Owned verified canonical tables loaded without source geometry parsing or mesh cooking. */
    struct LoadedPhysicsTriangleMesh final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        std::uint32_t schemaVersion{};
        std::uint32_t algorithmVersion{};
        Math::Aabb bounds;
        std::vector<Math::Vec3> vertices;
        std::vector<PhysicsMaterialSlotId> materialSlots;
        std::vector<LoadedPhysicsTriangle> triangles;
    };

    /**
     * @brief Validates normalized topology/material mappings and deterministically cooks one static triangle mesh.
     * @param request Exact asset/subresource, source tables, settings, target and diagnostic context.
     * @param cancellation Cooperative cancellation observed throughout bounded validation and encoding.
     * @return Canonical artifact bytes and exact descriptor, or a stable contextual validation/capacity/cancellation error.
     * @pre Offline authoring/cook work only; never call from scene activation or a runtime frame path.
     * @post Failure publishes nothing. Limits use fail-only policy; no triangle, vertex or material mapping is discarded.
     */
    [[nodiscard]] Result<PhysicsTriangleMeshCookResult> CookPhysicsTriangleMesh(const PhysicsTriangleMeshCookRequest &request,
                                                                                const CancellationToken &cancellation = {});

    /**
     * @brief Verifies and decodes one static triangle-mesh artifact without source parsing or cooking.
     * @param descriptor Exact catalog reference including cache-key, payload and target digests.
     * @param expectedTarget Runtime-captured complete Physics target identity.
     * @param payload Immutable cooked payload supplied by the Assets runtime provider.
     * @param limits Runtime allocation/count bounds; callers may lower but not raise qualified maxima.
     * @return Owned canonical geometry and stable per-triangle identity, or a stable integrity/compatibility/capacity error.
     */
    [[nodiscard]] Result<LoadedPhysicsTriangleMesh> LoadCookedPhysicsTriangleMesh(const PhysicsCookedShapeDescriptor &descriptor,
                                                                                  const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                                  std::span<const std::uint8_t> payload,
                                                                                  const PhysicsTriangleMeshCookLimits &limits = {});

    /**
     * @brief Enforces the CanonicalV1 static-only triangle-mesh body compatibility contract.
     * @param motion Requested body motion mode.
     * @return Success for Static, ShapeMotionUnsupported for Kinematic/Dynamic, or OperationUnsupported for an unknown mode.
     */
    [[nodiscard]] Result<void> ValidatePhysicsTriangleMeshMotion(PhysicsMotionType motion);
}  // namespace Horo::Physics
