#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "OwnedTestDirectory.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainViewportTool.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <imgui_internal.h>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    struct Project final {
        Horo::Tests::OwnedTestDirectory directory{"terrain workspace İstanbul"};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        Assets::AssetRegistry registry;
        Assets::AssetImporterCatalog catalog;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> importers;
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        std::unique_ptr<EditorWorkspaceController> controller;
        std::filesystem::path terrain, scene;

        Project() {
            scene = directory.Path() / "assets/scenes/main.horo";
            HoroEditorWorkspaceControllerFilesystemTests::PrepareEmptyDefaultScene(directory.Path(), scene);
            terrain = directory.Path() / "assets/land.horoasset";
            std::vector<std::byte> zeros(9 * 9 * 4);
            REQUIRE(files.WriteDurable(terrain, zeros).HasValue());
            const auto contribution = TerrainRawImporterContribution();
            Assets::AssetImportMetadata metadata;
            metadata.assetId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
            metadata.assetType = Assets::AssetTypeId::Parse("core.terrain").Value();
            metadata.importerContributionId = contribution.contributionId;
            metadata.importerVersion = contribution.version;
            metadata.importerPackageId = contribution.packageId;
            metadata.importerModuleId = contribution.moduleId;
            metadata.importerModuleVersion = contribution.moduleVersion;
            metadata.absoluteSourcePath = directory.Path() / "original.terrainraw";
            metadata.sourceExtension = "terrainraw";
            metadata.sourceHash = Assets::HashAssetImportSource(std::vector<std::uint8_t>(zeros.size()));
            metadata.sourceByteSize = zeros.size();
            metadata.importedAtUtc = "2026-10-31T00:00:00.000Z";
            metadata.importSettings = {{"settings.terrain-width", "9"},          {"settings.terrain-height", "9"},
                                       {"settings.terrain-spacing-x", "1"},      {"settings.terrain-spacing-z", "1"},
                                       {"settings.terrain-origin-x", "4000000"}, {"settings.terrain-origin-z", "-3000000"},
                                       {"settings.terrain-crs", "EPSG:32632"}};
            const auto serialized = Assets::SerializeAssetImportMetadata(metadata);
            REQUIRE(serialized.HasValue());
            auto sidecar = terrain;
            sidecar += ".horo";
            REQUIRE(files.WriteDurable(sidecar, std::as_bytes(std::span(serialized.Value()))).HasValue());
            REQUIRE(Assets::RebuildAssetRegistry(registry, directory.Path(), Assets::AssetRegistryOpenMode::ReadOnly).HasValue());
            REQUIRE(catalog.Register(contribution).HasValue());
            auto published = catalog.Publish();
            REQUIRE(published.HasValue());
            importers = std::move(published).Value();
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            EditorWorkspaceDependencies dependencies;
            dependencies.assets = {.mutableAssetRegistry = &registry, .importerCatalog = importers.get()};
            dependencies.world.documentSessionOwner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value();
            dependencies.mutations = &mutations;
            dependencies.durableFiles = &files;
            controller = std::make_unique<EditorWorkspaceController>(directory.Path(), runtime, registry.Snapshot(), dependencies);
            Open();
        }

        void Open() {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::OpenSourceFile;
            command.sourceOpenRequest = SourceOpenRequest{.path = terrain};
            controller->ProcessCommand(command);
            REQUIRE(controller->ViewModel().terrainDocuments.size() == 1);
        }

        std::shared_ptr<const TerrainDocumentHost> Host() const {
            return controller->ViewModel().terrainDocuments.front();
        }

        void Send(TerrainToolAction action, TerrainSculptMode mode = TerrainSculptMode::Raise,
                  std::optional<TerrainDocumentView> captured = {}) {
            auto intent = std::make_shared<TerrainToolCommand>();
            intent->document = captured ? *captured : Host()->View();
            intent->action = action;
            intent->brush.mode = mode;
            intent->brush.flattenMeters = 3;
            intent->stamps[0] = {4, 4};
            intent->stampCount = 1;
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::TerrainTool;
            command.terrainTool = std::move(intent);
            controller->ProcessCommand(command);
        }
    };
}  // namespace

TEST_CASE("Workspace opens real terrain assets and routes all five tools without authoring Scene mutation", "[terrain][workspace]") {
    Project project;
    const auto sceneBefore = Assets::CaptureAssetSource(project.directory.Path(), project.scene, 4096).Value().identity;
    const auto sceneRevision = project.controller->ViewModel().documentRevision;
    for (const auto mode : {TerrainSculptMode::Raise, TerrainSculptMode::Lower, TerrainSculptMode::Smooth, TerrainSculptMode::Flatten,
                            TerrainSculptMode::Noise}) {
        project.Send(TerrainToolAction::Stroke, mode);
        REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    }
    REQUIRE(project.controller->HasDirtySourceDocuments());
    REQUIRE(project.controller->ViewModel().documentRevision == sceneRevision);
    REQUIRE(Assets::CaptureAssetSource(project.directory.Path(), project.scene, 4096).Value().identity == sceneBefore);
    EditorWorkspaceViewCommandData save;
    save.command = EditorWorkspaceViewCommand::SaveAllSourceDocuments;
    project.controller->ProcessCommand(save);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    REQUIRE_FALSE(project.controller->HasDirtySourceDocuments());
    REQUIRE(Assets::CaptureAssetSource(project.directory.Path(), project.scene, 4096).Value().identity == sceneBefore);
}

TEST_CASE("Workspace dirty-close guards and retained terrain intents obey exact replacement fences", "[terrain][workspace]") {
    Project project;
    const auto original = project.Host();
    const auto old = original->View();
    project.Send(TerrainToolAction::Stroke);
    project.Send(TerrainToolAction::Stroke, TerrainSculptMode::Raise, old);
    REQUIRE(project.controller->ViewModel().terrainDocumentError.has_value());
    project.Send(TerrainToolAction::Close);
    REQUIRE(project.controller->ViewModel().terrainDocuments.size() == 1);
    REQUIRE(project.controller->ViewModel().terrainDocumentError.has_value());
    REQUIRE(project.controller->ShutdownTerrainDocuments().HasError());
    project.Send(TerrainToolAction::Discard);
    REQUIRE(project.controller->ViewModel().terrainDocuments.empty());
    REQUIRE(original->BorrowSource() == nullptr);
    project.Open();
    REQUIRE(project.Host()->View().instance != old.instance);
    project.Send(TerrainToolAction::Stroke, TerrainSculptMode::Raise, old);
    REQUIRE(project.controller->ViewModel().terrainDocumentError.has_value());
    REQUIRE_FALSE(project.Host()->View().dirty);
    REQUIRE(project.controller->ShutdownTerrainDocuments().HasValue());
}

TEST_CASE("Actual terrain overlay emits detached release commands and cancels camera replacement", "[terrain][gui]") {
    Project project;
    Editor::Tests::EditorGuiContextFixture gui;
    TerrainViewportTool tool;
    auto camera = project.controller->ViewModel().viewportCamera;
    const auto held = project.Host();
    std::optional<TerrainToolCommand> command;
    auto frame = [&](bool down, bool eligible = true, float width = 640) {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(320, 300);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        gui.imgui.BeginFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 700});
        ImGui::Begin("Terrain##test", nullptr, ImGuiWindowFlags_NoDecoration);
        command.reset();
        static_cast<void>(
            tool.Draw({*ImGui::GetWindowDrawList(), {0, 0}, {width, 600}, camera, Math::ClipDepthRange::ZeroToOne, {}, *held, eligible},
                      command, gui.context));
        ImGui::End();
        gui.imgui.EndFrame();
    };
    frame(false);
    frame(true);
    REQUIRE_FALSE(command.has_value());
    REQUIRE_FALSE(project.Host()->View().dirty);
    frame(false);
    REQUIRE(command.has_value());
    REQUIRE(command->action == TerrainToolAction::Stroke);
    REQUIRE(command->stampCount > 0);
    REQUIRE_FALSE(project.Host()->View().dirty);
    frame(true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    REQUIRE_FALSE(command.has_value());
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frame(false);
    frame(true);
    ImGui::GetIO().AddFocusEvent(false);
    frame(false);
    REQUIRE_FALSE(command.has_value());
    REQUIRE_FALSE(project.Host()->View().dirty);
    ImGui::GetIO().AddFocusEvent(true);
    frame(false);
    frame(true);
    camera.position.x += 1;
    frame(false);
    REQUIRE_FALSE(command.has_value());
    frame(true);
    frame(false, false);
    REQUIRE_FALSE(command.has_value());
    frame(true);
    frame(false, true, 300);
    REQUIRE_FALSE(command.has_value());
    frame(true);
    REQUIRE(project.controller->ShutdownTerrainDocuments(true).HasValue());
    frame(false);
    REQUIRE_FALSE(command.has_value());
}

TEST_CASE("Terrain controls render localized narrow popups at normal and enlarged UI scale", "[terrain][gui][localization]") {
    Project project;
    Editor::Tests::EditorGuiContextFixture gui;
    TerrainViewportTool tool;

    struct ResetScale final {
        ~ResetScale() {
            Theme::SetUiScalePercent(100);
        }
    } reset;

    const auto catalogRoot = std::filesystem::path{PROJECT_SOURCE_DIR} / "assets/localization/editor";
    REQUIRE(gui.localization.LoadCatalogFile(catalogRoot / "en-US.json"));
    REQUIRE(gui.localization.LoadCatalogFile(catalogRoot / "tr-TR.json"));
    const char *locale = "en-US";
    int scale = 100;
    SECTION("English") {}
    SECTION("Turkish narrow") {
        locale = "tr-TR";
    }
    SECTION("Turkish enlarged") {
        locale = "tr-TR";
        scale = 200;
    }
    REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
    REQUIRE(gui.localization.ActivatePrepared());
    REQUIRE(gui.localization.Get("editor", "terrain.tools.instructions").size() > 100);
    Theme::SetUiScalePercent(scale);
    gui.imgui.BeginFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({320, 700});
    ImGui::Begin("Terrain controls##localized", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::OpenPopup("##terrain_tools");
    std::optional<TerrainToolCommand> command;
    static_cast<void>(tool.Draw({*ImGui::GetWindowDrawList(),
                                 {0, 0},
                                 {300, 600},
                                 project.controller->ViewModel().viewportCamera,
                                 Math::ClipDepthRange::ZeroToOne,
                                 {},
                                 *project.Host(),
                                 true},
                                command, gui.context));
    REQUIRE_FALSE(command.has_value());
    REQUIRE(ImGui::IsPopupOpen("##terrain_tools"));
    REQUIRE_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    const auto *popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
    REQUIRE(popup != nullptr);
    REQUIRE(popup->Size.x <= 284.1F);
    REQUIRE(popup->Size.y <= 584.1F);
    ImGui::End();
    gui.imgui.EndFrame();
    REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
    REQUIRE_FALSE(project.Host()->View().dirty);
}
