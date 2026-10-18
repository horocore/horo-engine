#pragma once

#include "Horo/Assets/AssetRegistry.h"

namespace Horo::Assets {
    /** @brief Immutable revision and records shared by registry publications and borrowed snapshots. */
    struct AssetRegistrySnapshot::State {
        AssetRegistryRevision revision;
        std::vector<AssetRecord> records;
    };

}  // namespace Horo::Assets
