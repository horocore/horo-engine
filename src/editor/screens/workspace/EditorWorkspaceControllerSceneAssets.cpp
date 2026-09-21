#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <filesystem>

namespace Horo::Editor {

    void EditorWorkspaceController::LoadDocumentAssetMeshes() {
        for (const SceneObjectSnapshot &object : m_document.Objects()) {
            if (!object.meshAsset.has_value())
                continue;
            const Assets::AssetRecord *record = m_assetRegistry.Find(*object.meshAsset);
            if (record == nullptr || record->type.Value() != "core.mesh")
                continue;
            const std::filesystem::path source =
                (std::filesystem::path{m_viewModel.projectRoot} / record->sourcePath.String()).lexically_normal();
            const auto loaded = m_assetMeshCache.Load(record->id, source);
            if (loaded.HasError())
                LOG_WARN("editor.asset", "Unable to load scene mesh asset: %s", loaded.ErrorValue().message.c_str());
        }
    }

    void EditorWorkspaceController::HandleDuplicateObject(const SceneObjectId object) {
        const auto source = std::ranges::find(m_viewModel.objects, object, &SceneObject::id);
        if (source != m_viewModel.objects.end()) {
            HandleDocumentCommandResult(m_documentCommands.Execute(DuplicateSceneObjectCommand{source->id, source->name + " Copy"}),
                                        "Duplicate object");
        }
    }

    void EditorWorkspaceController::HandleDeleteObject(const SceneObjectId object) {
        HandleDeleteSelectedObjects({object});
    }

}  // namespace Horo::Editor
