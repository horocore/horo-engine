#include "../../helpers/editor_ui/HeadlessEditorGuiFixture.h"
#include "../support/AssetImportTestSupport.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Assets;

    using ScopedTempDirectory = ::Horo::Tests::ScopedAssetImportTempDirectory;

    /** @brief Test double that overrides Draw for headless testing. */
    class TestAssetImportModal : public AssetImportModal {
    public:
        using AssetImportModal::AssetImportModal;

        ModalFrameResult Draw() override {
            if (!m_preparedCalled && m_prepareFn) {
                m_prepareFn();
                m_preparedCalled = true;
            }
            return ModalFrameResult::None();
        }

        std::function<void()> m_prepareFn;
        bool m_preparedCalled{false};
    };

    [[nodiscard]] AssetImporterContribution BasicContribution() {
        return ::Horo::Tests::BasicAssetImporterContribution();
    }

    [[nodiscard]] AssetImporterContribution PresetContribution() {
        auto contribution = BasicContribution();
        contribution.contributionId = "test.mesh";
        contribution.moduleId = "mesh";
        contribution.fileExtensions = {"obj", "fbx"};
        contribution.settings = {
            ImportSettingDescriptor{.id = "optimize",
                                    .labelKey = "Optimize",
                                    .descriptionKey = "",
                                    .kind = ImportSettingKind::Boolean,
                                    .defaultValue = false,
                                    .includeInPresets = true},
            ImportSettingDescriptor{.id = "sourceTag",
                                    .labelKey = "Source Tag",
                                    .descriptionKey = "",
                                    .kind = ImportSettingKind::Text,
                                    .defaultValue = std::string{},
                                    .includeInPresets = false},
        };
        return contribution;
    }

    [[nodiscard]] std::shared_ptr<const AssetImporterCatalogSnapshot> PublishCatalog(AssetImporterContribution contribution) {
        auto published = ::Horo::Tests::PublishAssetImporterCatalog(std::move(contribution));
        REQUIRE(published != nullptr);
        return published;
    }

    void ConfigureFastPreset(AssetImportItem &item) {
        item.displayName = "PresetIndependentName";
        item.destinationFolder = "assets/Characters";
        item.subfolderByType = 2;
        item.assetIdStrategy = 1;
        item.createMetaSidecar = false;
        item.overwriteWithoutPrompt = true;
        item.settings["settings.optimize"] = "true";
        item.settings["settings.sourceTag"] = "first-source";
    }

    void ChangePresetFields(AssetImportItem &item) {
        item.displayName = "ChangedName";
        item.destinationFolder = "assets/Changed";
        item.subfolderByType = 0;
        item.assetIdStrategy = 0;
        item.createMetaSidecar = true;
        item.overwriteWithoutPrompt = false;
        item.settings["settings.optimize"] = "false";
        item.settings["settings.sourceTag"] = "second-source";
    }

    void CheckFastPreset(const AssetImportItem &item) {
        CHECK(item.displayName == "ChangedName");
        CHECK(item.destinationFolder == "assets/Characters");
        CHECK(item.subfolderByType == 2);
        CHECK(item.assetIdStrategy == 1);
        CHECK_FALSE(item.createMetaSidecar);
        CHECK(item.overwriteWithoutPrompt);
        CHECK(item.settings.at("settings.optimize") == "true");
        CHECK(item.settings.at("settings.sourceTag") == "second-source");
    }

    void CompleteImport(OperationStore &operations, const std::string &title) {
        const auto operation = operations.Begin(OperationDescriptor{
            .kind = OperationKind::Import,
            .title = title,
            .phase = "import",
            .message = "Importing assets",
            .progress = 0.0F,
        });
        REQUIRE(operation.has_value());
        REQUIRE(operations.Update(*operation, OperationUpdate{.state = OperationState::Succeeded,
                                                              .phase = "complete",
                                                              .message = "Asset import completed",
                                                              .progress = 1.0F}));
    }

}  // namespace

TEST_CASE("AssetImportModal lifecycle completes the visible operation before the modal closes", "[native]") {
    const ScopedTempDirectory project{"horo-asset-import-modal-lifecycle"};
    const auto sourceFile = project.Path() / "cube.obj";
    {
        std::ofstream source{sourceFile};
        source << "o cube";
    }

    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    const Theme::Fonts fonts{};
    JobSystem jobs;
    OperationStore operations{4, 4};

    auto modal = std::make_unique<TestAssetImportModal>(fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations);
    auto *modalPtr = modal.get();

    bool prepared = false;
    modalPtr->m_prepareFn = [modalPtr, &prepared]() {
        CancellationToken cancellation;
        auto result = modalPtr->ImportSingleItem(0, cancellation);
        REQUIRE((result.HasValue()));
        prepared = true;
    };

    auto openResult = modalHost.OpenRoot(std::move(modal));
    REQUIRE((openResult.HasValue()));

    // Open the modal to run OnOpen
    modalHost.OnUpdate(0.016f);

    // Begin import
    CancellationToken cancellation;
    auto beginResult = modalPtr->BeginImport({sourceFile}, project.Path(), cancellation);
    REQUIRE((beginResult.HasValue()));

    auto &snap = modalPtr->Snapshot();
    REQUIRE((snap.items.size() == 1));
    auto runningOperations = operations.SnapshotIfChanged(0);
    REQUIRE(runningOperations.has_value());
    REQUIRE((runningOperations->operations.size() == 1));
    REQUIRE((runningOperations->operations.front().state == OperationState::Running));

    // Draw triggers prepare
    modalHost.Draw();

    REQUIRE((prepared));
    REQUIRE(modalPtr->IsImportComplete());
    const auto completedOperations = operations.SnapshotIfChanged(runningOperations->revision);
    REQUIRE(completedOperations.has_value());
    REQUIRE((completedOperations->operations.front().state == OperationState::Succeeded));
    REQUIRE((completedOperations->operations.front().progress == 1.0F));

    // A stale cancel-style close request cannot relabel already committed work.
    auto closeResult = modalHost.RequestClose(modalPtr->Id(), ModalCloseReason::Cancelled);
    REQUIRE((closeResult.HasValue()));
    modalHost.OnUpdate(0.016f);
    REQUIRE_FALSE(operations.SnapshotIfChanged(completedOperations->revision).has_value());
}

TEST_CASE("AssetImportModal restores retained import history when reopened", "[native]") {
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    const Theme::Fonts fonts{};
    JobSystem jobs;
    OperationStore operations{4, 4};

    const auto operation = operations.Begin(OperationDescriptor{
        .kind = OperationKind::Import,
        .title = "sylvan_razorback",
        .phase = "import",
        .message = "Importing assets",
        .progress = 0.0F,
    });
    REQUIRE(operation.has_value());
    REQUIRE(operations.Update(*operation, OperationUpdate{.state = OperationState::Succeeded,
                                                          .phase = "complete",
                                                          .message = "Asset import completed",
                                                          .progress = 1.0F}));

    auto modal = std::make_unique<TestAssetImportModal>(fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations);
    auto *modalPtr = modal.get();
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);

    REQUIRE(modalPtr->Snapshot().items.empty());
    REQUIRE(modalPtr->ImportHistory().size() == 1);
    CHECK(modalPtr->ImportHistory().front().title == "sylvan_razorback");
    CHECK(modalPtr->ImportHistory().front().state == OperationState::Succeeded);
}

TEST_CASE("AssetImportModal keeps bounded incremental import history", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    OperationStore operations{64, 64};
    for (int index = 0; index < 55; ++index)
        CompleteImport(operations, std::format("import-{}", index));

    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    auto modal = std::make_unique<TestAssetImportModal>(fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations);
    auto *modalPtr = modal.get();
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);
    const auto &history = modalPtr->ImportHistory();
    REQUIRE(history.size() == 50);
    CHECK(history.front().title == "import-54");
    CHECK(history.back().title == "import-5");
}

TEST_CASE("AssetImportModal presets are scoped by importer contribution and extension", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    TestAssetImportModal modal{fonts, jobs, PublishCatalog(PresetContribution())};
    CancellationToken cancellation;
    REQUIRE((modal.BeginImport({"/tmp/test/cube.obj", "/tmp/test/character.fbx"}, "/tmp/test", cancellation).HasValue()));

    auto &objItem = modal.MutableSnapshot().items[0];
    ConfigureFastPreset(objItem);
    REQUIRE((modal.CreatePreset(0, "Fast")));
    REQUIRE((modal.ActivePresetName(0) == "Fast"));
    REQUIRE((modal.PresetNames(0) == std::vector<std::string>{"Default", "Fast"}));
    REQUIRE((modal.PresetNames(1) == std::vector<std::string>{"Default"}));

    ChangePresetFields(objItem);
    REQUIRE((modal.ApplyPreset(0, "Fast")));
    CheckFastPreset(objItem);
    REQUIRE((modal.ApplyPreset(0, "Default")));
    REQUIRE((objItem.displayName == "ChangedName"));
    REQUIRE((objItem.destinationFolder.empty()));
    REQUIRE((objItem.settings["settings.optimize"] == "false"));
    REQUIRE((objItem.settings["settings.sourceTag"] == "second-source"));
}

TEST_CASE("AssetImportModal applies an absolute Content Browser destination", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    const auto catalogSnapshot = PublishCatalog(BasicContribution());

    const ScopedTempDirectory project{"horo-import-destination"};
    const auto &projectRoot = project.Path();
    const std::filesystem::path destination = projectRoot / "assets/Meshes";
    std::filesystem::create_directories(destination);

    TestAssetImportModal modal{fonts, jobs, catalogSnapshot};
    modal.SetProjectRoot(projectRoot);
    modal.SetDefaultDestination(destination);
    CancellationToken cancellation;
    REQUIRE((modal.BeginImport({projectRoot / "source/cube.obj"}, projectRoot, cancellation).HasValue()));
    REQUIRE((modal.Snapshot().items.size() == 1));
    REQUIRE((modal.Snapshot().items[0].destinationFolder == "assets/Meshes"));
}

TEST_CASE("AssetImportModal does not duplicate an already selected type folder", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    AssetImporterCatalog catalog;
    REQUIRE((catalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.mesh",
                     .packageId = "test",
                     .moduleId = "mesh",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"fbx"},
                     .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                     .subfolderCategory = "Meshes",
                     .strategy = std::make_shared<const ::Horo::Tests::BasicAssetImporter>(),
                 })
                 .HasValue()));
    auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());

    const ScopedTempDirectory project{"horo-import-destination"};
    const auto &projectRoot = project.Path();
    const std::filesystem::path sourcePath = projectRoot / "source.fbx";
    std::filesystem::create_directories(projectRoot / "assets/Meshes");
    {
        std::ofstream source(sourcePath, std::ios::binary);
        source << "fbx";
    }

    TestAssetImportModal modal{fonts, jobs, catalogSnapshot.Value()};
    CancellationToken cancellation;
    REQUIRE((modal.BeginImport({sourcePath}, projectRoot, cancellation).HasValue()));
    auto &item = modal.MutableSnapshot().items[0];
    item.destinationFolder = "assets/Meshes";
    item.subfolderByType = 0;
    REQUIRE((modal.ImportSingleItem(0, cancellation).HasValue()));

    REQUIRE((std::filesystem::exists(projectRoot / "assets/Meshes/source.horoasset")));
    REQUIRE((!std::filesystem::exists(projectRoot / "assets/Meshes/Meshes")));
}

TEST_CASE("AssetImportModal tracks included queue items and appends files safely", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    OperationStore operations{4, 4};
    const ScopedTempDirectory project{"horo-import-inclusion"};
    const auto firstSource = project.Path() / "first.obj";
    const auto secondSource = project.Path() / "second.obj";
    {
        std::ofstream first{firstSource};
        first << "first";
        std::ofstream second{secondSource};
        second << "second";
    }

    TestAssetImportModal modal{fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations};
    modal.SetProjectRoot(project.Path());
    std::filesystem::create_directories(project.Path() / "assets/Imported");
    modal.SetDefaultDestination(project.Path() / "assets/Imported");
    CHECK(modal.DefaultDestinationFolder() == "assets/Imported");
    modal.SetDefaultDestination(project.Path() / "outside");
    CHECK(modal.DefaultDestinationFolder() == "assets/Imported");
    modal.SetDefaultDestination("assets/Imported");
    CHECK(modal.DefaultDestinationFolder() == "assets/Imported");

    CancellationToken cancellation;
    REQUIRE((modal.BeginImport({firstSource}, project.Path(), cancellation).HasValue()));
    REQUIRE((modal.BeginImport({secondSource}, project.Path(), cancellation).HasValue()));
    REQUIRE(modal.Snapshot().items.size() == 2);
    REQUIRE(modal.SourceFileSize(0).has_value());
    REQUIRE(modal.SourceFileSize(1).has_value());
    CHECK(modal.IncludedItemCount() == 2);

    modal.SetItemIncluded(0, false);
    modal.SetItemIncluded(1, false);
    modal.SetItemIncluded(99, true);
    CHECK_FALSE(modal.IsItemIncluded(0));
    CHECK_FALSE(modal.IsItemIncluded(1));
    CHECK(modal.IncludedItemCount() == 0);
    REQUIRE((modal.ImportIncludedItems(cancellation).HasValue()));
    CHECK(modal.IsImportComplete());
    REQUIRE((modal.ImportIncludedItems(cancellation).HasValue()));
    CHECK_FALSE(modal.SourceFileSize(99).has_value());

    TestAssetImportModal batchModal{fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations};
    REQUIRE((batchModal.BeginImport({firstSource, secondSource}, project.Path(), cancellation).HasValue()));
    const auto visibleOperations = operations.SnapshotIfChanged(0);
    REQUIRE(visibleOperations.has_value());
    CHECK(visibleOperations->operations.front().title == "first +1");
}

TEST_CASE("AssetImportModal rejects unresolved conflicts and invalid batch items", "[native]") {
    const Theme::Fonts fonts{};
    JobSystem jobs;
    const ScopedTempDirectory project{"horo-import-validation"};
    const auto source = project.Path() / "source.obj";
    {
        std::ofstream output{source};
        output << "source";
    }
    std::filesystem::create_directories(project.Path() / "assets");
    {
        std::ofstream existing{project.Path() / "assets/source.horoasset"};
        existing << "existing";
    }

    CancellationToken cancellation;
    TestAssetImportModal conflictModal{fonts, jobs, PublishCatalog(BasicContribution())};
    REQUIRE((conflictModal.BeginImport({source}, project.Path(), cancellation).HasValue()));
    REQUIRE((conflictModal.ImportSingleItem(0, cancellation).HasValue()));
    REQUIRE(conflictModal.HasPendingConflicts());
    const auto blocked = conflictModal.ImportIncludedItems(cancellation);
    REQUIRE(blocked.HasError());
    CHECK(blocked.ErrorValue().code.Value() == "editor.asset_import.conflict_pending");
    conflictModal.ResolveCurrentConflict(AssetImportModal::ConflictChoice::Skip, false);
    CHECK_FALSE(conflictModal.HasPendingConflicts());
    CHECK(conflictModal.IsImportComplete());

    TestAssetImportModal invalidModal{fonts, jobs, PublishCatalog(BasicContribution())};
    REQUIRE((invalidModal.BeginImport({source}, project.Path(), cancellation).HasValue()));
    invalidModal.MutableSnapshot().items.front().displayName.clear();
    const auto invalid = invalidModal.ImportIncludedItems(cancellation);
    REQUIRE(invalid.HasError());
    CHECK(invalid.ErrorValue().code.Value() == "editor.asset_import.invalid_asset_name");

    OperationStore operations{4, 4};
    TestAssetImportModal cancelledModal{fonts, jobs, PublishCatalog(BasicContribution()), nullptr, &operations};
    REQUIRE((cancelledModal.BeginImport({source}, project.Path(), cancellation).HasValue()));
    const auto visibleOperations = operations.SnapshotIfChanged(0);
    REQUIRE(visibleOperations.has_value());
    REQUIRE(visibleOperations->operations.front().requestCancel);
    visibleOperations->operations.front().requestCancel();
    CHECK(cancelledModal.ImportIncludedItems(cancellation).HasError());
}

TEST_CASE("AssetImportModal projects terminal import history while ignoring other operations", "[native]") {
    ::Horo::Editor::Tests::HeadlessEditorGuiFixture imgui;
    EditorDataBus events;
    Input::InputRouter inputRouter;
    EditorModalHost modalHost{events, inputRouter};
    ::Horo::Editor::Tests::ScopedJobSystem jobs;
    OperationStore operations{8, 8};

    const auto build = operations.Begin(OperationDescriptor{
        .kind = OperationKind::Build,
        .title = "build",
        .phase = "build",
        .message = "Building",
    });
    REQUIRE(build.has_value());
    REQUIRE(operations.Update(*build, OperationUpdate{.state = OperationState::Succeeded, .phase = "complete", .message = "Built"}));
    const auto import = operations.Begin(OperationDescriptor{
        .kind = OperationKind::Import,
        .title = "queued-import",
        .phase = "import",
        .message = "Importing",
    });
    REQUIRE(import.has_value());

    auto modal = std::make_unique<AssetImportModal>(imgui.Fonts(), jobs.Get(), PublishCatalog(BasicContribution()), nullptr, &operations);
    auto *const modalPtr = modal.get();
    REQUIRE(modalHost.OpenRoot(std::move(modal)).HasValue());
    modalHost.OnUpdate(0.016F);
    CHECK(modalPtr->ImportHistory().empty());

    REQUIRE(operations.Update(*import, OperationUpdate{.state = OperationState::Failed, .phase = "import", .message = "Import failed"}));
    imgui.BeginFrame();
    static_cast<void>(modalPtr->Draw());
    imgui.EndFrame();
    REQUIRE(modalPtr->ImportHistory().size() == 1);
    CHECK(modalPtr->ImportHistory().front().title == "queued-import");

    imgui.BeginFrame();
    static_cast<void>(modalPtr->Draw());
    imgui.EndFrame();
    CHECK(modalPtr->ImportHistory().size() == 1);
}
