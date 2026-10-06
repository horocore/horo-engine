#include "Horo/Terrain/TerrainMaterial.h"

#include <algorithm>
#include <numeric>

namespace Horo::Terrain {
    /** @copydoc TerrainMaterialLayerSet::Create */
    Result<TerrainMaterialLayerSet> TerrainMaterialLayerSet::Create(const TerrainMaterialLayerSetData &data,
                                                                    const TerrainConfigurationSnapshot &configuration) {
        if (!data.id.IsValid() || !data.revision.IsValid() || data.layerCount == 0 ||
            data.layerCount > TerrainDescriptorHardLimits::LayersPerTile)
            return Result<TerrainMaterialLayerSet>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        if (data.layerCount > configuration.Data().limits.maximumLayersPerTile)
            return Result<TerrainMaterialLayerSet>::Failure(MakeError(TerrainErrors::LimitExceeded));
        for (std::size_t index = 0; index < data.layers.size(); ++index) {
            const auto &layer = data.layers[index];
            if (index >= data.layerCount) {
                if (layer != TerrainMaterialLayer{})
                    return Result<TerrainMaterialLayerSet>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                continue;
            }
            if (!layer.id.IsValid() || !layer.material.IsValid() || layer.uvScaleMilli == 0 || layer.uvScaleMilli > 1'000'000)
                return Result<TerrainMaterialLayerSet>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (data.layers[previous].id == layer.id)
                    return Result<TerrainMaterialLayerSet>::Failure(MakeError(TerrainErrors::IdentityConflict));
            }
        }
        return Result<TerrainMaterialLayerSet>::Success(TerrainMaterialLayerSet{data});
    }

    /** @copydoc TerrainMaterialLayerSet::Data */
    const TerrainMaterialLayerSetData &TerrainMaterialLayerSet::Data() const noexcept {
        return data_;
    }

    /** @copydoc NormalizeTerrainMaterialWeights */
    Result<TerrainMaterialWeights> NormalizeTerrainMaterialWeights(const std::span<const std::uint32_t> weights) {
        if (weights.empty() || weights.size() > TerrainDescriptorHardLimits::LayersPerTile)
            return Result<TerrainMaterialWeights>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        const auto sum = std::accumulate(weights.begin(), weights.end(), std::uint64_t{0});
        if (sum == 0)
            return Result<TerrainMaterialWeights>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        TerrainMaterialWeights result;
        result.layerCount = static_cast<std::uint8_t>(weights.size());
        std::array<std::uint64_t, TerrainDescriptorHardLimits::LayersPerTile> remainders{};
        std::uint32_t assigned = 0;
        for (std::size_t index = 0; index < weights.size(); ++index) {
            const std::uint64_t numerator = static_cast<std::uint64_t>(weights[index]) * 65'535U;
            result.values[index] = static_cast<std::uint16_t>(numerator / sum);
            remainders[index] = numerator % sum;
            assigned += result.values[index];
        }
        while (assigned < 65'535U) {
            const auto best = std::max_element(remainders.begin(), remainders.begin() + static_cast<std::ptrdiff_t>(weights.size()));
            ++result.values[static_cast<std::size_t>(best - remainders.begin())];
            *best = 0;
            ++assigned;
        }
        return Result<TerrainMaterialWeights>::Success(result);
    }

    /** @copydoc ValidateTerrainMaterialWeights */
    Result<void> ValidateTerrainMaterialWeights(const TerrainMaterialWeights &weights, const TerrainMaterialLayerSet &layers,
                                                const std::uint16_t tileLayerMask) {
        const auto count = layers.Data().layerCount;
        const auto knownMask = (std::uint32_t{1} << count) - 1U;
        if (weights.layerCount != count || tileLayerMask == 0 || (tileLayerMask & ~knownMask) != 0)
            return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        std::uint32_t sum = 0;
        for (std::size_t index = 0; index < weights.values.size(); ++index) {
            if ((index >= count || (tileLayerMask & (std::uint32_t{1} << index)) == 0) && weights.values[index] != 0)
                return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            sum += weights.values[index];
        }
        if (sum != 65'535U)
            return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
