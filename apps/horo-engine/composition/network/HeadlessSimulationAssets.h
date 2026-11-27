#pragma once

/** @file HeadlessSimulationAssets.h @brief Selects cooked payloads used by dedicated simulation owners. */
#include "CommittedSceneAssets.h"
#include "Horo/Prefab/PrefabLimits.h"

namespace Horo::Application::Internal {
    /** @brief Roots and catalogs selected by the application before dedicated Scene activation. */
    struct HeadlessSimulationAssets final {
        std::vector<Assets::AssetId> decisions;
        bool runtimePrefabs{};
    };

    /** @brief Keeps typed Scene dependencies and opted-in gameplay catalogs from the verified generation. */
    [[nodiscard]] Result<std::vector<Assets::AssetId>> SelectHeadlessSimulationAssets(
        const CommittedSceneAssets &committed, const Runtime::RuntimeSceneDefinition &scene, Assets::AssetId cookedScene,
        const HeadlessSimulationAssets &selection, const Prefab::PrefabProjectPolicy &prefabPolicy, const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
