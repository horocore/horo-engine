#pragma once
#include "Horo/Editor/TerrainAuthoringDocument.h"
#include "Horo/Terrain/TerrainDescriptor.h"

namespace Horo::Editor {
    /** @brief Prevent owner callback reentry until a complete host command has returned. */
    struct TerrainHostBusy final {
        bool &value;

        explicit TerrainHostBusy(bool &busy) noexcept : value(busy) {
            value = true;
        }

        ~TerrainHostBusy() {
            value = false;
        }
    };

    /** @brief One explicit reservation profile used both for source opening and converted owner replacement. */
    inline TerrainEditLimits TerrainHostEditLimits() {
        TerrainEditLimits limits;
        limits.maximumPatches = 1;
        limits.maximumSamples = 16'384;
        limits.maximumTransactionBytes = 512 * 1024;
        limits.maximumHistoryBytes = 8 * 1024 * 1024;
        limits.maximumHistoryItems = 64;
        return limits;
    }

    /** @brief Explicit tooling-only validation profile; this does not activate any runtime capability. */
    inline Terrain::TerrainConfigurationSnapshot TerrainHostConfiguration() {
        using namespace Terrain;
        return TerrainConfigurationSnapshot::Create({.configuration = TerrainConfigurationRevision::Create(1).Value(),
                                                     .capability = TerrainCapabilityRevision::Create(1).Value(),
                                                     .tier = TerrainFeatureTier::Ultra,
                                                     .limits = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value().limits})
            .Value();
    }
}  // namespace Horo::Editor
