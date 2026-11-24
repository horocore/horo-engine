#pragma once

#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <filesystem>
#include <functional>
#include <span>
#include <vector>

namespace Horo::Editor {
    struct EnvironmentQueryPreviewNavigationRequest final {
        const std::filesystem::path &root;
        Assets::AssetId sceneAsset;
        const Runtime::RuntimeSceneDefinition &definition;
        const Runtime::RuntimeSceneView &scene;
        Runtime::EntityRef querier;
        Math::WorldCoordinate64 start;
        AI::QueryStageId stage;
    };

    /** @brief Detaches authoring path work from the pinned Scene without opening a live Navigation caller. */
    struct EnvironmentQueryPreviewNavigation final {
        static constexpr std::size_t MaximumPaths = 32;
        using Evaluate =
            std::function<Result<std::vector<AI::QueryTestSample>>(std::span<const Math::WorldCoordinate64>, const CancellationToken &)>;
        std::function<Result<Evaluate>(const EnvironmentQueryPreviewNavigationRequest &)> capture;
    };
}  // namespace Horo::Editor
