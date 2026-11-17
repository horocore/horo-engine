#pragma once
/** @file EditorTerrainPreparation.h @brief Executable-owned native terrain source and offline cook composition. */
#include "Horo/Application/TerrainAuthoringAssets.h"
#include "Horo/Application/TerrainAuthoringCook.h"

namespace Horo::Editor {
    /** @brief Bind actual shared services; returned factories and every cook session are drained before these services retire. */
    [[nodiscard]] std::shared_ptr<const Application::TerrainAuthoringCookFactory> MakeEditorTerrainCookFactory(
        JobSystem &jobs, Assets::AssetRegistry &assets, ProjectMutationCoordinator &mutations, DurableFileSystem &files,
        OperationStore &operations, BuildOutputStore &output);
    /** @brief Bind source creation to the same shared registry/writer/files authority as ordinary project imports. */
    [[nodiscard]] std::shared_ptr<const Application::TerrainAuthoringAssetFactory> MakeEditorTerrainAssetFactory(
        Assets::AssetRegistry &assets, ProjectMutationCoordinator &mutations, DurableFileSystem &files);
}  // namespace Horo::Editor
