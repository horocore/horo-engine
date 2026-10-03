#include "../support/AssetImportTestSupport.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Runtime/Input.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <imgui_internal.h>
#include <memory>

namespace {
    std::vector<Horo::Assets::ImportSettingDescriptor> MakeCatalogSettings() {
        using namespace Horo::Assets;
        return {
            {.id = "optimize",
             .labelKey = "Optimize",
             .descriptionKey = "Optimize mesh data",
             .kind = ImportSettingKind::Boolean,
             .defaultValue = true},
            {.id = "importMaterials",
             .labelKey = "Generate Materials",
             .descriptionKey = "Generate material assets",
             .kind = ImportSettingKind::Boolean,
             .defaultValue = true},
            {.id = "importAnimations",
             .labelKey = "Animation Import",
             .descriptionKey = "Import animation tracks",
             .kind = ImportSettingKind::Boolean,
             .defaultValue = false},
            {.id = "lod-count",
             .labelKey = "LOD count",
             .descriptionKey = "Generated detail levels",
             .kind = ImportSettingKind::Integer,
             .defaultValue = std::int64_t{3}},
            {.id = "scale", .labelKey = "Scale", .descriptionKey = "Import scale", .kind = ImportSettingKind::Float, .defaultValue = 1.0},
            {.id = "tag",
             .labelKey = "Tag",
             .descriptionKey = "Source tag",
             .kind = ImportSettingKind::Text,
             .defaultValue = std::string{"environment"}},
            {.id = "normals",
             .labelKey = "Normals",
             .descriptionKey = "Normal generation policy",
             .kind = ImportSettingKind::Choice,
             .defaultValue = std::size_t{0},
             .choices = {{.id = "source", .labelKey = "Source", .value = std::size_t{0}},
                         {.id = "generate", .labelKey = "Generate", .value = std::size_t{1}}}},
        };
    }

    std::shared_ptr<const Horo::Assets::AssetImporterCatalogSnapshot> MakeCatalog() {
        using namespace Horo::Assets;

        AssetImporterContribution contribution{
            .contributionId = "horo.builtin.obj-mesh",
            .packageId = "horo.builtin.assets",
            .moduleId = "horo.assets.obj",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"obj", "fbx", "png", "wav"},
            .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
            .settings = MakeCatalogSettings(),
            .builtIn = true,
        };
        return std::make_shared<const AssetImporterCatalogSnapshot>(std::vector<AssetImporterContribution>{std::move(contribution)});
    }

    Horo::Assets::AssetImportItem MakeItem() {
        using namespace Horo;
        using namespace Horo::Assets;

        AssetImportItem item{
            .sourceFile = ProjectPath::Parse("source/scene.obj").Value(),
            .absoluteSourcePath = "/tmp/source/scene.obj",
        };
        item.importerContributionId = "horo.builtin.obj-mesh";
        item.importerVersion = "1.0.0";
        item.importerPackageId = "horo.builtin.assets";
        item.importerModuleId = "horo.assets.obj";
        item.importerModuleVersion = "1.0.0";
        item.resolvedType = AssetTypeId::Parse("core.mesh").Value();
        item.sourceExtension = "obj";
        item.displayName = "scene";
        item.destinationFolder = "assets/Meshes";
        item.diagnostics = {
            {.severity = ImportDiagnostic::Severity::Info, .code = "mesh.info", .message = "scene.obj: source metadata read"},
            {.severity = ImportDiagnostic::Severity::Warning,
             .code = "mesh.warning",
             .message = "scene: missing tangents; generation requested"},
            {.severity = ImportDiagnostic::Severity::Error, .code = "mesh.error", .message = "Malformed optional group"},
        };
        return item;
    }

    void DrawFrame(Horo::Editor::Tests::HeadlessEditorGuiFixture &imgui, Horo::Editor::AssetImportModal &modal) {
        imgui.BeginFrame();
        static_cast<void>(modal.Draw());
        imgui.EndFrame();
    }

    struct AssetImportPresentationFixture {
        AssetImportPresentationFixture() : modal{imgui.Fonts(), jobs.Get(), MakeCatalog()} {}

        Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        Horo::Editor::AssetImportModal modal;
    };

    void ExpandAdvancedSection() {
        for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
            if (std::string_view{window->Name}.find("/ImportDetails_") != std::string_view::npos) {
                window->StateStorage.SetInt(window->GetID("Advanced"), 1);
                return;
            }
        }
        FAIL("Import details child was not drawn");
    }

}  // namespace

TEST_CASE("Asset import presentation renders queue diagnostics settings destination and terminal phases",
          "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Editor;

    AssetImportPresentationFixture fixture;
    auto &snapshot = fixture.modal.MutableSnapshot();
    snapshot.items = {MakeItem()};
    snapshot.phase = AssetImportPhase::Selecting;
    snapshot.canCancel = true;
    snapshot.items.front().settings["settings.lod-count"] = "invalid";
    snapshot.items.front().settings["settings.scale"] = "invalid";
    snapshot.items.front().settings["settings.normals"] = "invalid";

    DrawFrame(fixture.imgui, fixture.modal);
    ExpandAdvancedSection();
    DrawFrame(fixture.imgui, fixture.modal);

    snapshot.items.front().result = PreparedAssetImport{.type = AssetTypeId::Parse("core.mesh").Value(), .editorPayload = {1, 2, 3}};
    snapshot.items.front().diagnostics.clear();
    static constexpr std::array phases{AssetImportPhase::Preparing, AssetImportPhase::ReadyToCommit, AssetImportPhase::Committing,
                                       AssetImportPhase::Completed, AssetImportPhase::Failed,        AssetImportPhase::Cancelled};
    for (const AssetImportPhase phase : phases) {
        snapshot.phase = phase;
        DrawFrame(fixture.imgui, fixture.modal);
    }

    REQUIRE(snapshot.items.size() == 1);
    REQUIRE(snapshot.items.front().displayName == "scene");
    REQUIRE(snapshot.items.front().result.has_value());
}

TEST_CASE("Asset import presentation handles empty and unresolved importer selections", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Editor;

    AssetImportPresentationFixture fixture;
    DrawFrame(fixture.imgui, fixture.modal);

    auto unresolved = MakeItem();
    unresolved.sourceFile = ProjectPath::Parse("source/material.unknown").Value();
    unresolved.sourceExtension = "unknown";
    unresolved.importerContributionId.clear();
    fixture.modal.MutableSnapshot().items = {std::move(unresolved)};
    fixture.modal.MutableSnapshot().selectedItemIndex = 0;
    DrawFrame(fixture.imgui, fixture.modal);

    REQUIRE(fixture.modal.Snapshot().items.front().importerContributionId.empty());
}

TEST_CASE("Asset import settings round trip typed values and reject malformed or completed edits", "[unit][editor][asset-import]") {
    using namespace Horo::Assets;
    AssetImportPresentationFixture fixture;
    auto &snapshot = fixture.modal.MutableSnapshot();
    snapshot.items = {MakeItem()};
    const auto settings = MakeCatalogSettings();
    const std::array<ImportSettingValue, 7> values{false, true, true, std::int64_t{8}, 2.5, std::string{"custom"}, std::size_t{1}};
    for (std::size_t index = 0; index < settings.size(); ++index) {
        fixture.modal.SetSettingValue(0, settings[index], values.at(index));
        REQUIRE(fixture.modal.SettingValue(0, settings[index]) == values.at(index));
    }

    SECTION("malformed numbers restore defaults") {
        for (const std::size_t index : {3U, 4U, 6U}) {
            auto &raw = snapshot.items.front().settings["settings." + settings[index].id];
            raw = "invalid";
            REQUIRE(fixture.modal.SettingValue(0, settings[index]) == settings[index].defaultValue);
            raw = index == 4U ? "1e9999" : "9999999999999999999999999999999999999999999999999999999999999999999";
            REQUIRE(fixture.modal.SettingValue(0, settings[index]) == settings[index].defaultValue);
        }
        snapshot.items.front().settings["settings.scale"] = "1e9999";
        REQUIRE(fixture.modal.SettingValue(0, settings[4]) == settings[4].defaultValue);
        snapshot.items.front().settings["settings.scale"] = "2.5trailing";
        REQUIRE(fixture.modal.SettingValue(0, settings[4]) == settings[4].defaultValue);
    }
    SECTION("completed items retain their settings") {
        snapshot.items.front().result = PreparedAssetImport{.type = AssetTypeId::Parse("core.mesh").Value()};
        const auto before = snapshot.items.front().settings;
        fixture.modal.SetSettingValue(0, settings[5], std::string{"changed"});
        REQUIRE(snapshot.items.front().settings == before);
    }
    SECTION("missing items return defaults without mutation") {
        const auto before = snapshot.items.front().settings;
        fixture.modal.SetSettingValue(99, settings[5], std::string{"changed"});
        REQUIRE(fixture.modal.SettingValue(99, settings[5]) == settings[5].defaultValue);
        REQUIRE(snapshot.items.front().settings == before);
    }
}

TEST_CASE("Asset import presentation formats captured kilobyte source sizes", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Editor;

    ::Horo::Tests::ScopedAssetImportTempDirectory project{"horo-presentation-size"};
    const auto source = project.Path() / "scene.obj";
    {
        std::ofstream output{source};
        output << std::string(2048, 'x');
    }

    AssetImportPresentationFixture fixture;
    CancellationToken cancellation;
    REQUIRE((fixture.modal.BeginImport({source}, project.Path(), cancellation).HasValue()));
    DrawFrame(fixture.imgui, fixture.modal);
    REQUIRE(fixture.modal.SourceFileSize(0).value() == 2048);
}

TEST_CASE("Asset import presentation renders retained history status variants", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Editor;

    Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    Horo::Editor::Tests::ScopedJobSystem jobs;
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    OperationStore operations{8, 8};

    static constexpr std::array states{OperationState::Succeeded, OperationState::Failed, OperationState::Cancelled};
    for (const OperationState state : states) {
        const auto operation = operations.Begin(OperationDescriptor{
            .kind = OperationKind::Import,
            .title = "history-entry",
            .phase = "import",
            .message = "Importing assets",
        });
        REQUIRE(operation.has_value());
        REQUIRE(operations.Update(*operation, OperationUpdate{
                                                  .state = state,
                                                  .phase = "complete",
                                                  .message = "Import finished",
                                              }));
    }

    auto modal = std::make_unique<AssetImportModal>(imgui.Fonts(), jobs.Get(), MakeCatalog(),
                                                    AssetImportModalServices{.operationStore = &operations});
    auto *const modalPtr = modal.get();
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);
    REQUIRE(modalPtr->ImportHistory().size() == states.size());

    DrawFrame(imgui, *modalPtr);
}
