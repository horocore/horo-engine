#pragma once

#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <fstream>

namespace HoroEditorWorkspaceControllerFilesystemTests {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Seeds the existing default-scene persistence contract before the workspace owner starts. */
    inline void PrepareEmptyDefaultScene(const std::filesystem::path &projectRoot, const std::filesystem::path &scenePath) {
        std::filesystem::create_directories(projectRoot / ".horo");
        std::filesystem::create_directories(scenePath.parent_path());
        std::ofstream metadata(projectRoot / ".horo/project.json", std::ios::binary);
        metadata << R"({"settings":{"defaultScene":"assets/scenes/main.horo"}})";
        std::ofstream scene(scenePath, std::ios::binary);
        scene << "{\"schemaVersion\":1,\"objects\":[]}\n";
    }

    /** @brief Supplies the same typed box-creation intent used by workspace history and persistence scenarios. */
    inline EditorWorkspaceViewCommandData BoxCreationCommand() {
        EditorWorkspaceViewCommandData create;
        create.command = EditorWorkspaceViewCommand::CreatePrimitive;
        create.primitivePayload = Runtime::PrimitiveId{"primitive.mesh.box"};
        return create;
    }

    /** @brief Constructs a payload-free scene intent while each scenario owns dispatch and observation. */
    inline EditorWorkspaceViewCommandData SceneCommand(EditorWorkspaceViewCommand operation) {
        EditorWorkspaceViewCommandData command;
        command.command = operation;
        return command;
    }

    /** @brief Writes the canonical mesh identity sidecar paired with the test payload. */
    inline void WriteIdentityAsset(const std::filesystem::path &source, const std::string_view assetId) {
        std::ofstream payload(source, std::ios::binary);
        payload << "asset";
        std::ofstream metadata(source.string() + ".horo");
        metadata << "{\n  \"schemaVersion\": 1,\n  \"assetId\": \"" << assetId << "\",\n  \"assetType\": \"core.mesh\"\n}";
    }

    /** @brief Creates a complete on-disk asset pair whose malformed metadata forces a degraded registry rebuild. */
    inline void WriteCorruptAsset(const std::filesystem::path &source) {
        std::ofstream payload(source, std::ios::binary);
        payload << "asset";
        std::ofstream metadata(source.string() + ".horo");
        metadata << "{ invalid";
    }

    // Fault injection and attempt accounting share one target-private fixture; no duplicate filesystem protocol.
    using SyncFailingFilesystem = HoroEditorWorkspaceControllerPolicyTests::SyncFailingFilesystem;

}  // namespace HoroEditorWorkspaceControllerFilesystemTests
