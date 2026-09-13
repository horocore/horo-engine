#pragma once

/**
 * @file PhysicsConvexHullCook.h
 * @brief Deterministic bounded convex-hull cooking and source-free runtime loading.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsCookedShapeDescriptor.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Physics {
    /** @brief Explicit over-limit policy; the baseline never simplifies or truncates collision topology. */
    enum class PhysicsConvexHullLimitPolicy : std::uint8_t {
        Fail,
        Count,
    };

    /** @brief Qualified count and payload limits for one convex-hull cook. */
    struct PhysicsConvexHullCookLimits final {
        static constexpr std::uint32_t MaximumSourceVertices = 1'000'000;
        static constexpr std::uint32_t MaximumHullVertices = 256;
        static constexpr std::uint64_t MaximumPayloadBytes = 256ULL * 1024ULL * 1024ULL;

        std::uint32_t maxSourceVertices{65'536};
        std::uint32_t maxHullVertices{MaximumHullVertices};
        std::uint64_t maxPayloadBytes{MaximumPayloadBytes};

        [[nodiscard]] constexpr auto operator<=>(const PhysicsConvexHullCookLimits &) const noexcept = default;
    };

    /** @brief Versioned convex-hull settings that participate in cook identity. */
    struct PhysicsConvexHullCookSettings final {
        static constexpr std::uint32_t CurrentSchemaVersion = 1;
        static constexpr std::uint32_t CurrentAlgorithmVersion = 1;

        std::uint32_t schemaVersion{CurrentSchemaVersion};
        std::uint32_t algorithmVersion{CurrentAlgorithmVersion};
        PhysicsConvexHullLimitPolicy limitPolicy{PhysicsConvexHullLimitPolicy::Fail};
        PhysicsConvexHullCookLimits limits;

        [[nodiscard]] constexpr auto operator<=>(const PhysicsConvexHullCookSettings &) const noexcept = default;
    };

    /**
     * @brief Immutable normalized-meter input captured by the offline Physics shape cooker.
     *
     * Vertex order and canonical binary32 values are the source bytes for sourceDigest and cook-key
     * construction. The caller retains ownership for the duration of the synchronous call. sourceContext
     * is diagnostic evidence only and never participates in identity or artifact bytes.
     */
    struct PhysicsConvexHullCookRequest final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId subresource;
        std::span<const Math::Vec3> vertices;
        PhysicsConvexHullCookSettings settings;
        PhysicsShapeCookTargetDigest target;
        std::string_view sourceContext;
    };

    /** @brief Complete deterministic cook output ready for Assets envelope/cache publication. */
    struct PhysicsConvexHullCookResult final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        Math::Aabb bounds;
        std::uint32_t sourceVertexCount{};
        std::uint32_t hullVertexCount{};
        std::uint32_t triangleCount{};
        std::vector<std::uint8_t> payload;
    };

    /** @brief Owned verified canonical hull tables loaded without source geometry or hull generation. */
    struct LoadedPhysicsConvexHull final {
        PhysicsCookedShapeDescriptor descriptor;
        Sha256Digest sourceDigest;
        std::uint32_t schemaVersion{};
        std::uint32_t algorithmVersion{};
        Math::Aabb bounds;
        std::vector<Math::Vec3> vertices;
        std::vector<std::uint32_t> triangleIndices;
    };

    /**
     * @brief Validates normalized vertices and deterministically cooks one closed convex hull.
     * @param request Exact asset/subresource, source vertices, settings, target and diagnostic context.
     * @param cancellation Cooperative cancellation observed during bounded hull construction.
     * @return Canonical artifact bytes and exact descriptor, or a stable contextual validation/capacity/cancellation error.
     * @pre Offline authoring/cook work only; never call from scene activation or a runtime frame path.
     * @post Failure publishes nothing and does not mutate input. Limits use fail-only policy; no vertex or face is silently removed.
     */
    [[nodiscard]] Result<PhysicsConvexHullCookResult> CookPhysicsConvexHull(const PhysicsConvexHullCookRequest &request,
                                                                            const CancellationToken &cancellation = {});

    /**
     * @brief Verifies and decodes one cooked convex artifact without source parsing or hull generation.
     * @param descriptor Exact catalog reference including cache-key, payload and target digests.
     * @param expectedTarget Runtime-captured complete Physics target identity.
     * @param payload Immutable cooked payload bytes supplied by the Assets runtime provider.
     * @param limits Runtime allocation/count bounds; callers may lower but not raise qualified maxima.
     * @return Owned canonical vertices/triangles, or a stable integrity/compatibility/capacity error.
     */
    [[nodiscard]] Result<LoadedPhysicsConvexHull> LoadCookedPhysicsConvexHull(const PhysicsCookedShapeDescriptor &descriptor,
                                                                              const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                              std::span<const std::uint8_t> payload,
                                                                              const PhysicsConvexHullCookLimits &limits = {});
}  // namespace Horo::Physics
