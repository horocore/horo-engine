#include "../support/AssetImportTestSupport.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Runtime/Input.h"
#include "editor/ui_preview/AssetImportPreviewModal.h"
#include "editor/ui_preview/EditorUiPreviewCatalog.h"
#include "editor/ui_preview/EditorUiPreviewGallery.h"
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

    void ClickTab(Horo::Editor::Tests::HeadlessEditorGuiFixture &imgui, Horo::Editor::AssetImportModal &modal, const std::size_t tabIndex) {
        static constexpr std::array labels{"Overview", "Diagnostics", "Importer Settings", "Destination"};
        REQUIRE(tabIndex < labels.size());
        const ImGuiWindow *const window = ImGui::FindWindowByName("Asset Import");
        REQUIRE(window != nullptr);

        constexpr float horizontalPadding = 16.0F;
        constexpr float tabGap = 2.0F;
        float tabOffset = 22.0F;
        for (std::size_t index = 0; index < tabIndex; ++index)
            tabOffset += horizontalPadding * 2.0F + ImGui::CalcTextSize(labels[index]).x + tabGap;
        const float tabWidth = horizontalPadding * 2.0F + ImGui::CalcTextSize(labels[tabIndex]).x;

        ImGuiIO &io = ImGui::GetIO();
        io.AddMousePosEvent(window->Pos.x + tabOffset + tabWidth * 0.5F, window->Pos.y + 44.0F + 64.0F + 24.0F);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        DrawFrame(imgui, modal);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        DrawFrame(imgui, modal);
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
    ClickTab(fixture.imgui, fixture.modal, 1);
    ClickTab(fixture.imgui, fixture.modal, 2);
    ClickTab(fixture.imgui, fixture.modal, 3);

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
    ClickTab(fixture.imgui, fixture.modal, 2);

    REQUIRE(fixture.modal.Snapshot().items.front().importerContributionId.empty());
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

    auto modal = std::make_unique<AssetImportModal>(imgui.Fonts(), jobs.Get(), MakeCatalog(), nullptr, &operations);
    auto *const modalPtr = modal.get();
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);
    REQUIRE(modalPtr->ImportHistory().size() == states.size());

    DrawFrame(imgui, *modalPtr);
}

TEST_CASE("Asset import preview fixtures render populated and empty workflow states", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    Horo::Editor::Tests::ScopedJobSystem jobs;
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};

    auto modal = std::make_unique<AssetImportPreviewModal>(imgui.Fonts(), jobs.Get(), MakeCatalog());
    auto *const modalPtr = modal.get();
    modalPtr->SetScenario(AssetImportPreviewScenario::Populated);
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);

    REQUIRE(modalPtr->IsReadOnlyPresentation());
    REQUIRE(modalPtr->Snapshot().items.size() == 4);
    REQUIRE(modalPtr->SourceFileSize(0).value() == 12'400'000);
    DrawFrame(imgui, *modalPtr);

    for (std::size_t index = 0; index < modalPtr->Snapshot().items.size(); ++index) {
        modalPtr->SelectItem(index);
        ClickTab(imgui, *modalPtr, 1);
        ClickTab(imgui, *modalPtr, 2);
        ClickTab(imgui, *modalPtr, 3);
    }

    REQUIRE(modalPtr->Snapshot().items[0].sourceExtension == "fbx");
    REQUIRE(modalPtr->Snapshot().items[1].sourceExtension == "png");
    REQUIRE(modalPtr->Snapshot().items[3].diagnostics.size() == 1);
}

TEST_CASE("Asset import preview fixture renders an empty queue", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    Horo::Editor::Tests::ScopedJobSystem jobs;
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};

    auto modal = std::make_unique<AssetImportPreviewModal>(imgui.Fonts(), jobs.Get(), MakeCatalog());
    auto *const modalPtr = modal.get();
    modalPtr->SetScenario(AssetImportPreviewScenario::Empty);
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);
    DrawFrame(imgui, *modalPtr);

    REQUIRE(modalPtr->IsReadOnlyPresentation());
    REQUIRE(modalPtr->Snapshot().items.empty());
}

TEST_CASE("Editor UI preview gallery renders both interaction states", "[unit][editor][gui][ui-preview]") {
    using namespace Horo;
    using namespace Horo::Editor;

    Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    LocalizationService localization{LocaleTag{"en-US"}};

    imgui.BeginFrame();
    const auto initialSelection = DrawEditorUiPreviewGallery("asset-import-empty", true, imgui.Fonts(), localization);
    imgui.EndFrame();
    REQUIRE_FALSE(initialSelection.has_value());

    const ImGuiWindow *const gallery = ImGui::FindWindowByName("##EditorUiPreviewGallery");
    REQUIRE(gallery != nullptr);
    const float firstButtonTop = EditorUiPreviewHeaderHeight + 54.0F;
    const float buttonHeight = 38.0F;
    const float secondButtonCenter = firstButtonTop + buttonHeight + ImGui::GetStyle().ItemSpacing.y + buttonHeight * 0.5F;
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(gallery->Pos.x + EditorUiPreviewSidebarWidth * 0.5F, gallery->Pos.y + secondButtonCenter);
    imgui.BeginFrame();
    const auto hoveredSelection = DrawEditorUiPreviewGallery("asset-import-empty", false, imgui.Fonts(), localization);
    imgui.EndFrame();
    REQUIRE_FALSE(hoveredSelection.has_value());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    imgui.BeginFrame();
    const auto clickedSelection = DrawEditorUiPreviewGallery("asset-import-empty", false, imgui.Fonts(), localization);
    imgui.EndFrame();
    REQUIRE_FALSE(clickedSelection.has_value());
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    imgui.BeginFrame();
    const auto releasedSelection = DrawEditorUiPreviewGallery("asset-import-empty", false, imgui.Fonts(), localization);
    imgui.EndFrame();
    REQUIRE(releasedSelection.has_value());
    REQUIRE(*releasedSelection == "asset-import");
}

TEST_CASE("Asset import preview fixtures expose deterministic populated and empty states", "[unit][editor][gui][asset-import]") {
    using namespace Horo;
    using namespace Horo::Assets;
    using namespace Horo::Editor;

    {
        Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
        Horo::Editor::Tests::ScopedJobSystem jobs;
        EditorDataBus events;
        Input::InputRouter inputRouter;
        EditorModalHost modalHost{events, inputRouter};
        auto modal = std::make_unique<AssetImportPreviewModal>(imgui.Fonts(), jobs.Get(), MakeCatalog());
        auto *const modalPtr = modal.get();
        modalPtr->SetScenario(AssetImportPreviewScenario::Populated);
        REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
        modalHost.OnUpdate(0.016F);

        REQUIRE(modalPtr->IsReadOnlyPresentation());
        REQUIRE(modalPtr->Snapshot().items.size() == 4);
        REQUIRE(modalPtr->Snapshot().items.front().displayName == "hero");
        REQUIRE(modalPtr->Snapshot().items.back().diagnostics.size() == 1);
        DrawFrame(imgui, *modalPtr);
    }

    Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    Horo::Editor::Tests::ScopedJobSystem jobs;
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    auto modal = std::make_unique<AssetImportPreviewModal>(imgui.Fonts(), jobs.Get(), MakeCatalog());
    auto *const modalPtr = modal.get();
    modalPtr->SetScenario(AssetImportPreviewScenario::Empty);
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);

    REQUIRE(modalPtr->IsReadOnlyPresentation());
    REQUIRE(modalPtr->Snapshot().items.empty());
    DrawFrame(imgui, *modalPtr);
}
