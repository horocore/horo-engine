#include "EditorWorkspaceController.h"
#include "EditorWorkspaceScreen.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "WorkspacePathText.h"
#include "terrain/TerrainAssetCreationModal.h"
#include "terrain/TerrainDocumentHost.h"

#include <algorithm>

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @brief Freeze the selected document and actual resource choices for one exclusive creation modal. */
    void EditorWorkspaceScreen::OpenTerrainAssetCreationModal(const EditorWorkspaceViewCommandData &command) {
        if (!command.documentInstance || !controller_)
            return;
        const auto &model = controller_->ViewModel();
        const auto found = std::ranges::find_if(model.terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == *command.documentInstance;
        });
        if (found == model.terrainDocuments.end() || !model.terrainAssetCreationAvailable)
            return;
        const auto captured = (*found)->View();
        auto choices = controller_->CaptureTerrainAssetChoices();
        if (choices.HasError()) {
            LOG_WARN("editor.terrain", "%s", choices.ErrorValue().message.c_str());
            return;
        }
        const auto kind = command.command == EditorWorkspaceViewCommand::CreateTerrainLayerSet
                              ? Application::TerrainAuthoringAssetKind::LayerSet
                              : Application::TerrainAuthoringAssetKind::FoliageType;
        auto opened = modalHost_.OpenRoot(
            std::make_unique<TerrainAssetCreationModal>(context_, kind, controller_->ProjectRoot(), std::move(choices).Value(),
                                                        [controller = controller_.get(), captured,
                                                         lifetime = std::weak_ptr<const void>{workspaceControllerLifetime_}](
                                                            Application::TerrainAuthoringAssetRequest request) {
            using Return = Result<std::shared_ptr<const Application::TerrainAuthoringAssetCreated>>;
            if (lifetime.expired())
                return Return::Failure(MakeError(TerrainEditErrors::Closed));
            return controller->CreateTerrainAsset(captured, request);
        }));
        if (opened.HasError())
            LOG_WARN("editor.terrain", "%s", opened.ErrorValue().message.c_str());
    }
}  // namespace Horo::Editor::WorkspaceScreenDetail
