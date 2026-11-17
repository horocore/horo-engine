#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "../runtime/terrain/FoliageAuthoringTestSupport.h"
#include "EditorWorkspaceControllerFilesystemTestSupport.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "OwnedTestDirectory.h"
#include "terrain/FoliagePaletteTab.h"
#include "terrain/TerrainAssetCreationModal.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainToolSelection.h"
#include "terrain/TerrainViewportTool.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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
        Application::TerrainAuthoringAssetFactory assetFactory;
        std::uint16_t nextIdentity{1};
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
            assetFactory = [this](const Application::TerrainAuthoringAssetRequest &request) {
                return Application::CreateTerrainAuthoringAsset({registry, mutations, files,
                                                                 [this] {
                    std::array<std::uint8_t, 16> bytes{};
                    bytes[0] = 0x99;
                    bytes[14] = static_cast<std::uint8_t>(nextIdentity >> 8);
                    bytes[15] = static_cast<std::uint8_t>(nextIdentity++);
                    return Result<Assets::AssetId>::Success(Assets::AssetId::FromBytes(bytes));
                }, Terrain::FoliageAuthoringTest::Configuration()},
                                                                request);
            };
            EditorWorkspaceDependencies dependencies;
            dependencies.assets = {.mutableAssetRegistry = &registry, .importerCatalog = importers.get()};
            dependencies.world.documentSessionOwner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000002").Value();
            dependencies.terrainAssets = &assetFactory;
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

    void PublishFoliageType(Project &project) {
        using namespace Terrain::FoliageAuthoringTest;
        const auto type = Type();
        auto encoded = Terrain::EncodeFoliageTypeSource(type, Configuration(), Capabilities());
        REQUIRE(encoded.HasValue());
        Application::PreparedAuthoredProjectAssetBatch batch{.operationId = "workspace-foliage-fixture",
                                                             .projectRoot = project.directory.Path(),
                                                             .items = {{Asset(3),
                                                                        Assets::AssetTypeId::Parse("core.mesh").Value(),
                                                                        ProjectPath::Parse("assets/mesh.horoasset").Value(),
                                                                        {std::byte{0x31}}},
                                                                       {Asset(4),
                                                                        Assets::AssetTypeId::Parse("core.material").Value(),
                                                                        ProjectPath::Parse("assets/material.horoasset").Value(),
                                                                        {std::byte{0x32}}},
                                                                       {type.asset, Assets::AssetTypeId::Parse("core.foliage.type").Value(),
                                                                        ProjectPath::Parse("assets/grass.horoasset").Value(),
                                                                        std::move(encoded).Value()}}};
        Application::ProjectAssetImportCommitter publisher{project.mutations, project.files, &project.registry};
        REQUIRE(publisher.CommitAuthored(batch).HasValue());
        project.controller->RefreshAssets(project.registry.Snapshot());
        project.Send(TerrainToolAction::RefreshFoliage);
        REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    }

    void SendFoliage(Project &project, TerrainToolMode mode) {
        const auto &state = project.controller->ViewModel().terrainToolSelection;
        REQUIRE(state);
        REQUIRE(state->palette.Active());
        auto intent = std::make_shared<TerrainToolCommand>();
        intent->document = project.Host()->View();
        intent->mode = mode;
        intent->foliageType = state->palette.Active()->source;
        intent->foliage.radiusMeters = 1;
        intent->foliage.mode = mode == TerrainToolMode::FoliageErase ? FoliagePaintMode::Erase : FoliagePaintMode::Paint;
        intent->stamps[0] = {4, 4};
        intent->stampCount = 1;
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::TerrainTool;
        command.terrainTool = std::move(intent);
        project.controller->ProcessCommand(command);
        REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    }
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

TEST_CASE("Workspace explicit conversion routes hole edits and preserves saved source across reopen",
          "[terrain][workspace][layered-tools]") {
    Project project;
    const auto legacy = project.Host()->View();
    project.Send(TerrainToolAction::ConvertSource);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    REQUIRE(project.Host()->View().containerSource);
    CHECK(project.Host()->View().fence.session != legacy.fence.session);
    auto sendHole = [&](TerrainToolMode mode, TerrainPaintMode paintMode) {
        auto intent = std::make_shared<TerrainToolCommand>();
        intent->action = TerrainToolAction::Stroke;
        intent->document = project.Host()->View();
        intent->mode = mode;
        intent->paint.mode = paintMode;
        intent->paint.radiusMeters = 1;
        intent->paintFence.document = intent->document.fence;
        intent->stamps[0] = {4, 4};
        intent->stampCount = 1;
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::TerrainTool;
        command.terrainTool = std::move(intent);
        project.controller->ProcessCommand(command);
    };
    sendHole(TerrainToolMode::CarveHole, TerrainPaintMode::CarveHole);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    REQUIRE(project.Host()->BorrowSource()->holes[40] == 1);
    const auto carved = project.Host()->BorrowSource()->holes;
    project.Send(TerrainToolAction::Undo);
    REQUIRE(project.Host()->BorrowSource()->holes[40] == 0);
    project.Send(TerrainToolAction::Redo);
    REQUIRE(project.Host()->BorrowSource()->holes == carved);
    project.Send(TerrainToolAction::Save);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    CHECK_FALSE(project.Host()->View().dirty);
    project.Send(TerrainToolAction::Close);
    REQUIRE(project.controller->ViewModel().terrainDocuments.empty());
    project.Open();
    REQUIRE(project.Host()->BorrowSource()->holes == carved);
    sendHole(TerrainToolMode::RestoreHole, TerrainPaintMode::RestoreHole);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    CHECK(project.Host()->BorrowSource()->holes[40] == 0);
    project.Send(TerrainToolAction::Stroke, TerrainSculptMode::Raise, legacy);
    REQUIRE(project.controller->ViewModel().terrainDocumentError.has_value());
    CHECK(project.Host()->BorrowSource()->holes[40] == 0);
}

TEST_CASE("Workspace actual foliage palette paints selects erases and reopens exact authored identities",
          "[terrain][workspace][foliage-tools]") {
    Project project;
    project.Send(TerrainToolAction::ConvertSource);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    PublishFoliageType(project);
    const auto state = project.controller->ViewModel().terrainToolSelection;
    REQUIRE(state);
    REQUIRE(state->palette.Items().size() == 1);
    auto choose = std::make_shared<TerrainToolCommand>();
    choose->document = project.Host()->View();
    choose->action = TerrainToolAction::ChooseFoliage;
    choose->foliageType = state->palette.Items()[0].source;
    EditorWorkspaceViewCommandData command;
    command.command = EditorWorkspaceViewCommand::TerrainTool;
    command.terrainTool = std::move(choose);
    project.controller->ProcessCommand(command);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    SendFoliage(project, TerrainToolMode::FoliagePaint);
    const auto placements = project.Host()->BorrowDocument()->Placements();
    REQUIRE(placements.size() == 5);
    const auto revision = project.Host()->View().fence.expectedRevision;
    SendFoliage(project, TerrainToolMode::FoliagePaint);
    CHECK(project.Host()->BorrowDocument()->Placements() == placements);
    CHECK(project.Host()->View().fence.expectedRevision == revision);
    SendFoliage(project, TerrainToolMode::FoliageSelect);
    CHECK(project.controller->ViewModel().terrainToolSelection->selection.Ids().size() == 5);
    CHECK(project.Host()->View().fence.expectedRevision == revision);
    SendFoliage(project, TerrainToolMode::FoliageErase);
    REQUIRE(project.Host()->BorrowDocument()->Placements().empty());
    project.Send(TerrainToolAction::Undo);
    REQUIRE(project.Host()->BorrowDocument()->Placements() == placements);
    project.Send(TerrainToolAction::Save);
    REQUIRE_FALSE(project.controller->ViewModel().terrainDocumentError.has_value());
    project.Send(TerrainToolAction::Close);
    REQUIRE(project.controller->ViewModel().terrainDocuments.empty());
    CHECK(state->selection.Ids().empty());
    project.Open();
    CHECK(project.Host()->BorrowDocument()->Placements() == placements);
    CHECK(project.controller->ViewModel().terrainToolSelection->selection.Ids().empty());
}

TEST_CASE("Workspace resource creation publishes actual definitions and fences late document replacement",
          "[terrain][workspace][resource-creation]") {
    Project project;
    PublishFoliageType(project);
    using namespace Terrain::FoliageAuthoringTest;
    Application::TerrainAuthoringAssetRequest layers{project.directory.Path(),
                                                     ProjectPath::Parse("assets/Created.terrainlayers").Value(),
                                                     Application::TerrainAuthoringAssetKind::LayerSet,
                                                     {Asset(4), Asset(4)},
                                                     {}};
    const auto captured = project.Host()->View();
    auto created = project.controller->CreateTerrainAsset(captured, layers);
    REQUIRE(created.HasValue());
    REQUIRE(created.Value()->publication.registryPublished);
    REQUIRE_FALSE(created.Value()->failure);
    const auto layerRegistry = project.registry.Snapshot();
    const auto *layerRecord = layerRegistry.Find(created.Value()->asset);
    REQUIRE(layerRecord);
    CHECK(layerRecord->type.Value() == "core.terrain.layerset");
    CHECK(layerRecord->sourcePath.String() == layers.destination.String());
    REQUIRE(project.Host()->BorrowLayerSets().size() == 1);
    Application::TerrainAuthoringAssetRequest type{project.directory.Path(),
                                                   ProjectPath::Parse("assets/Created.foliagetype").Value(),
                                                   Application::TerrainAuthoringAssetKind::FoliageType,
                                                   {},
                                                   Type().definition};
    auto definition = project.controller->CreateTerrainAsset(project.Host()->View(), type);
    REQUIRE(definition.HasValue());
    REQUIRE(definition.Value()->publication.registryPublished);
    REQUIRE_FALSE(definition.Value()->failure);
    const auto typeRegistry = project.registry.Snapshot();
    const auto *typeRecord = typeRegistry.Find(definition.Value()->asset);
    REQUIRE(typeRecord);
    CHECK(typeRecord->type.Value() == "core.foliage.type");
    CHECK(typeRecord->sourcePath.String() == type.destination.String());
    const auto state = project.controller->ViewModel().terrainToolSelection;
    REQUIRE(state->palette.Active());
    CHECK(state->palette.Active()->source.asset == definition.Value()->asset);
    const auto actualFactory = project.assetFactory;
    project.assetFactory = [&](const auto &request) {
        auto published = actualFactory(request);
        project.Send(TerrainToolAction::Close);
        return published;
    };
    layers.destination = ProjectPath::Parse("assets/Late.terrainlayers").Value();
    auto late = project.controller->CreateTerrainAsset(project.Host()->View(), layers);
    REQUIRE(late.HasValue());
    REQUIRE(late.Value()->publication.registryPublished);
    REQUIRE(late.Value()->failure);
    CHECK(project.controller->ViewModel().terrainAssetCreationOutcome == late.Value());
    CHECK(project.controller->ViewModel().terrainDocuments.empty());
    CHECK(project.registry.Snapshot().Find(late.Value()->asset));
}

TEST_CASE("Actual foliage palette and resource modal render localized bounded drafts and cancel without publication",
          "[terrain][gui][resource-creation]") {
    const auto locale = GENERATE("en-US", "tr-TR");
    const auto kind = GENERATE(Application::TerrainAuthoringAssetKind::LayerSet, Application::TerrainAuthoringAssetKind::FoliageType);
    Project project;
    PublishFoliageType(project);
    Editor::Tests::EditorGuiContextFixture gui{{360, 640}};
    const auto catalogs = std::filesystem::path{PROJECT_SOURCE_DIR} / "assets/localization/editor";
    REQUIRE(gui.localization.LoadCatalogFile(catalogs / "en-US.json"));
    REQUIRE(gui.localization.LoadCatalogFile(catalogs / "tr-TR.json"));
    REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
    REQUIRE(gui.localization.ActivatePrepared());
    FoliagePaletteTab palette;
    EditorWorkspaceViewCommandData command;
    gui.imgui.BeginFrame();
    ImGui::Begin("Foliage palette##render");
    palette.DrawPanel({0, 0}, {340, 600}, project.controller->ViewModel(), command, gui.context);
    ImGui::End();
    gui.imgui.EndFrame();
    CHECK(command.command == EditorWorkspaceViewCommand::None);
    auto choices = project.controller->CaptureTerrainAssetChoices();
    REQUIRE(choices.HasValue());
    REQUIRE(choices.Value().size() == 2);
    EditorModalHost host{gui.editorEvents, gui.input};
    std::size_t submissions{};
    REQUIRE(
        host.OpenRoot(std::make_unique<TerrainAssetCreationModal>(gui.context, kind, project.directory.Path(), std::move(choices).Value(),
                                                                  [&](Application::TerrainAuthoringAssetRequest) {
        ++submissions;
        return Result<std::shared_ptr<const Application::TerrainAuthoringAssetCreated>>::Failure(MakeError(TerrainEditErrors::Invalid));
    })).HasValue());
    for (int frame = 0; frame < 2; ++frame) {
        gui.imgui.BeginFrame();
        host.Draw();
        gui.imgui.EndFrame();
    }
    REQUIRE(ImGui::GetDrawData());
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    REQUIRE(host.RequestCloseAllForShutdown().HasValue());
    CHECK(submissions == 0);
    CHECK_FALSE(project.Host()->View().dirty);
}
