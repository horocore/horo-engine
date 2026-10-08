#pragma once

/** @file TerrainMaterial.h
 * @brief Ordered semantic terrain layers and canonical UNORM16 weight blending.
 */

#include "Horo/Terrain/TerrainDescriptor.h"

#include <array>
#include <span>

namespace Horo::Terrain {
    struct TerrainLayerIdentityTag;
    struct TerrainLayerSetIdentityTag;
    struct TerrainMaterialAssetIdentityTag;
    /** @brief Stable semantic layer identity, independent of shader or array indices. */
    using TerrainLayerId = TerrainStableIdentity<TerrainLayerIdentityTag>;
    /** @brief Stable ordered layer-set asset identity. */
    using TerrainLayerSetId = TerrainStableIdentity<TerrainLayerSetIdentityTag>;
    /** @brief Material-domain asset reference; texture and function semantics belong to that material. */
    using TerrainMaterialAssetId = TerrainStableIdentity<TerrainMaterialAssetIdentityTag>;

    /** @brief One authored semantic layer referencing a standard material asset. */
    struct TerrainMaterialLayer final {
        TerrainLayerId id{};
        TerrainMaterialAssetId material{};
        std::uint32_t uvScaleMilli{1000}; /**< Positive UV repeat scale in thousandths, at most 1,000,000. */
        [[nodiscard]] constexpr auto operator<=>(const TerrainMaterialLayer &) const noexcept = default;
    };

    /** @brief Fixed-size immutable-content construction value; unused slots must be zero/default. */
    struct TerrainMaterialLayerSetData final {
        TerrainLayerSetId id{};
        TerrainContentRevision revision{};
        std::uint8_t layerCount{};
        std::array<TerrainMaterialLayer, TerrainDescriptorHardLimits::LayersPerTile> layers{};
        [[nodiscard]] constexpr auto operator<=>(const TerrainMaterialLayerSetData &) const noexcept = default;
    };

    /** @brief Validated ordered layer meanings, owned by value and independent of renderer state. */
    class TerrainMaterialLayerSet final {
    public:
        /**
         * @brief Validates identities, uniqueness, canonical storage and exact tier limits.
         * @param data Ordered authored/cooked layer meanings.
         * @param configuration Captured exact tier and project limits.
         * @return Owned layer set or a typed failure; required excess never clamps or truncates.
         */
        [[nodiscard]] static Result<TerrainMaterialLayerSet> Create(const TerrainMaterialLayerSetData &data,
                                                                    const TerrainConfigurationSnapshot &configuration);
        /** @brief Returns immutable owned semantic data. @return Borrowed value for this object's lifetime. */
        [[nodiscard]] const TerrainMaterialLayerSetData &Data() const noexcept;

    private:
        explicit TerrainMaterialLayerSet(const TerrainMaterialLayerSetData &data) noexcept : data_(data) {}

        TerrainMaterialLayerSetData data_;
    };

    /** @brief Canonical ordered weights for one sample; populated weights sum to exactly 65535. */
    struct TerrainMaterialWeights final {
        std::uint8_t layerCount{};
        std::array<std::uint16_t, TerrainDescriptorHardLimits::LayersPerTile> values{};
        [[nodiscard]] constexpr auto operator<=>(const TerrainMaterialWeights &) const noexcept = default;
    };

    /**
     * @brief Normalizes finite unsigned source weights using exact integer largest remainders.
     * @param weights One to sixteen weights in authored layer order; ties prefer earlier layers.
     * @return Fixed-size canonical UNORM16 sample or typed invalid input for empty/oversized/zero totals.
     * @details Allocation-free synchronous work with no retained state or lifecycle side effects.
     */
    [[nodiscard]] Result<TerrainMaterialWeights> NormalizeTerrainMaterialWeights(std::span<const std::uint32_t> weights);
    /**
     * @brief Validates a canonical sample against the selected layer set and legal per-tile layer mask.
     * @param weights Ordered canonical sample with zero unused slots.
     * @param layers Exact admitted semantic layer set.
     * @param tileLayerMask Non-empty subset of the layer set; excluded weights must be zero.
     * @return Success or a typed invalid sample/mask failure without modification.
     */
    [[nodiscard]] Result<void> ValidateTerrainMaterialWeights(const TerrainMaterialWeights &weights, const TerrainMaterialLayerSet &layers,
                                                              std::uint16_t tileLayerMask);
}  // namespace Horo::Terrain
