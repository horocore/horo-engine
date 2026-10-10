#pragma once

/** @file LightFrameLayout.h
 * @brief Explicit 32-bit packed light/cluster ABI for the cooked light-culling kernel.
 */
#include "Horo/Runtime/Render/LightCulling.h"

#include <type_traits>

namespace Horo::Render {
    /** @brief Four 16-byte groups; integer kind and identity never cross the shader ABI as floats. */
    struct PackedRenderLight final {
        std::array<float, 4> positionRange;
        std::array<float, 3> direction;
        std::uint32_t kind{};
        std::array<float, 4> colorIntensity;
        std::array<float, 2> cones;
        std::uint32_t identityLow{};
        std::uint32_t identityHigh{};
    };

    /** @brief Six float4 plane equations in the same order as the CPU reference. */
    struct PackedLightCluster final {
        std::array<std::array<float, 4>, 6> planes;
    };

    /** @brief 16-byte output record; fourth lane prevents uint3 alignment differences between target languages. */
    struct PackedLightMembership final {
        std::uint32_t offset{};
        std::uint32_t count{};
        std::uint32_t omitted{};
        std::uint32_t reserved{};
    };

    /** @brief Exact finite dispatch parameters; shader interprets coverage through omitted-count evidence. */
    struct LightCullingDispatch final {
        std::uint32_t lightCount{};
        std::uint32_t clusterCount{};
        std::uint32_t referencesPerCluster{};
        std::uint32_t reserved{};
    };

    static_assert(sizeof(PackedRenderLight) == 64 && std::is_trivially_copyable_v<PackedRenderLight>);
    static_assert(offsetof(PackedRenderLight, kind) == 28 && offsetof(PackedRenderLight, cones) == 48);
    static_assert(sizeof(PackedLightCluster) == 96 && std::is_trivially_copyable_v<PackedLightCluster>);
    static_assert(sizeof(PackedLightMembership) == 16 && sizeof(LightCullingDispatch) == 16);

    /**
     * @brief Converts canonical valid light and cluster values into the documented shader ABI without allocation.
     * @param lights Strictly increasing nonzero stable identities and valid light values.
     * @param clusters Valid six-plane cluster volumes.
     * @param packedLights Caller-owned output storage at least lights.size() long.
     * @param packedClusters Caller-owned output storage at least clusters.size() long.
     * @return Success or typed invalid/capacity failure; validation precedes all writes.
     */
    [[nodiscard]] Result<void> PackLightFrame(std::span<const IdentifiedRenderLight> lights, std::span<const LightCluster> clusters,
                                              std::span<PackedRenderLight> packedLights, std::span<PackedLightCluster> packedClusters);
}  // namespace Horo::Render
