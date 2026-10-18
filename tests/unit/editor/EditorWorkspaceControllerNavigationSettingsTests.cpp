#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "Horo/Editor/NavigationProjectSettingsSession.h"
#include "support/OwnedTestDirectory.h"

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace HoroEditorWorkspaceControllerFilesystemTests;

    /** @brief Registers the real default Scene bytes before the workspace opens its canonical navigation owner. */
    void RegisterScene(Assets::AssetRegistry &registry) {
        const auto asset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
        const auto source = ProjectPath::Parse("assets/scenes/main.horo").Value();
        const auto metadata = ProjectPath::Parse("assets/scenes/main.horo.horo").Value();
        REQUIRE(registry.Publish({{asset, Assets::AssetTypeId::Parse("core.scene").Value(), source, metadata}}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
    }
}  // namespace

TEST_CASE("Workspace scene copies and rejected Save As preserve the exact navigation opening", "[unit][editor][navigation][persistence]") {
    Tests::OwnedTestDirectory directory{"navigation-scene-destination"};
    const auto root = directory.Path();
    const auto original = root / "assets/scenes/main.horo";
    PrepareEmptyDefaultScene(root, original);
    Assets::AssetRegistry registry;
    RegisterScene(registry);
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations{files};
    CancellationSource cancellation;
    Runtime::RuntimeSceneService runtime;
    REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
    EditorWorkspaceController controller{root,
                                         runtime,
                                         registry.Snapshot(),
                                         {.mutableAssetRegistry = &registry, .mutations = &mutations, .durableFiles = &files}};
    const auto opened = controller.NavigationSettings();
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value() != nullptr);
    const auto borrow = opened.Value()->Borrow();
    auto copy = SceneCommand(EditorWorkspaceViewCommand::SaveSceneCopyAs);
    const auto copyPath = root / "assets/scenes/copy.horo";
    copy.stringPayload = copyPath.string();
    controller.ProcessCommand(copy);
    REQUIRE(std::filesystem::exists(copyPath));
    REQUIRE(borrow.Resolve() == opened.Value());
    REQUIRE(controller.CurrentScenePath() == std::filesystem::weakly_canonical(original));
    auto rename = SceneCommand(EditorWorkspaceViewCommand::SaveSceneAs);
    for (const auto &invalid : {root / "assets/scenes/invalid.txt", root.parent_path() / "outside.horo"}) {
        rename.stringPayload = invalid.string();
        controller.ProcessCommand(rename);
        REQUIRE_FALSE(std::filesystem::exists(invalid));
        REQUIRE(borrow.Resolve() == opened.Value());
    }
    const auto renamedPath = root / "assets/scenes/renamed.horo";
    rename.stringPayload = renamedPath.string();
    controller.ProcessCommand(rename);
    REQUIRE(std::filesystem::exists(renamedPath));
    REQUIRE(borrow.Resolve() == nullptr);
    REQUIRE(controller.CurrentScenePath() == std::filesystem::weakly_canonical(renamedPath));
}
