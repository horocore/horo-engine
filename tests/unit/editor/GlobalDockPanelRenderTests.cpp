#include "ContentBrowserModel.h"
#include "Horo/Assets/MeshEditorPayload.h"
#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Logging/StructuredLogStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "editor/input/EditorScrollSmoother.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPaneLayout.h"
#include "editor/screens/workspace/panels/global_dock/panes/build_output/GlobalDockBuildOutputPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/operations/GlobalDockOperationsPane.h"
#include "runtime/assets/importer/builtin/obj_mesh/ObjMeshImporter.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
    class TestGlobalDockPane final : public Horo::Editor::IGlobalDockPane {
    public:
        [[nodiscard]] std::string_view Id() const noexcept override {
            return "test.global_dock.custom";
        }

        [[nodiscard]] std::string_view LabelKey() const noexcept override {
            return "workspace.global_dock.tab.custom";
        }

        void Draw(const Horo::Editor::GlobalDockPaneDrawContext &) override {}
    };

    class TestLocalization final : public Horo::Editor::ILocalizationService {
    public:
        [[nodiscard]] const std::string &Get(const std::string_view, const std::string_view localKey) const override {
            std::string_view value = localKey;
            if (localKey == "workspace.global_dock.tab.assets")
                value = "Assets";
            else if (localKey == "workspace.global_dock.tab.console")
                value = "Console";
            else if (localKey == "workspace.global_dock.tab.mcp")
                value = "MCP";
            else if (localKey == "workspace.global_dock.tab.performance")
                value = "Perf";
            else if (localKey == "workspace.global_dock.tab.physics")
                value = "Physics";
            else if (localKey == "workspace.global_dock.tab.audio")
                value = "Audio";
            else if (localKey == "workspace.global_dock.tab.network")
                value = "Net";
            else if (localKey == "workspace.global_dock.tab.localization")
                value = "L10n";

            const auto [entry, inserted] = values_.try_emplace(std::string(localKey), value);
            static_cast<void>(inserted);
            return entry->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> values_;
    };

    TEST_CASE("Content browser grid metrics respond to available width", "[unit][editor][gui]") {
        using Horo::Editor::ComputeAssetBrowserGridMetrics;

        REQUIRE(Horo::Editor::GlobalDockLabelFontSize() == Horo::Editor::Theme::TextPx::Label());
        REQUIRE(Horo::Editor::AssetBrowserLayout::SecondaryFontSize() == Horo::Editor::Theme::TextPx::Caption());

        const auto wide = ComputeAssetBrowserGridMetrics(580.0F);
        REQUIRE((wide.columns == 3));
        REQUIRE((std::abs(wide.cardWidth - 152.0F) < 0.001F));

        const auto narrow = ComputeAssetBrowserGridMetrics(240.0F);
        REQUIRE((narrow.columns == 1));
        REQUIRE((std::abs(narrow.cardWidth - 152.0F) < 0.001F));
    }

    TEST_CASE("Global dock exposes the default tabs", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        const std::array expected{
            GlobalDockTab::Assets,      GlobalDockTab::Console, GlobalDockTab::BuildOutput, GlobalDockTab::Operations, GlobalDockTab::Mcp,
            GlobalDockTab::Performance, GlobalDockTab::Physics, GlobalDockTab::Audio,       GlobalDockTab::Network,
        };
        REQUIRE((DefaultGlobalDockTabs() == expected));

        GlobalDockPanel panel;
        REQUIRE((panel.ActiveTab() == GlobalDockTab::Assets));
        REQUIRE((panel.ActivePaneId() == "horo.global_dock.assets"));
        REQUIRE(panel.RegisterPane(std::make_unique<TestGlobalDockPane>()));
        REQUIRE_FALSE(panel.RegisterPane(std::make_unique<TestGlobalDockPane>()));
        REQUIRE(panel.ActivatePane("test.global_dock.custom"));
        REQUIRE((panel.ActivePaneId() == "test.global_dock.custom"));
        REQUIRE_FALSE(panel.ActivatePane("test.global_dock.missing"));
    }

    TEST_CASE("Global dock layout partitions optional regions without overlap", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        const GlobalDockPaneRegions regions =
            ResolveGlobalDockPaneRegions({10.0F, 20.0F}, 800.0F, 300.0F, {.hasToolbar = true, .hasFooter = true, .leftRailWidth = 42.0F});
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        REQUIRE((regions.contentOrigin.x == 52.0F));
        REQUIRE((regions.contentOrigin.y == 20.0F + metrics.toolbarHeight));
        REQUIRE((regions.contentWidth == 758.0F));
        REQUIRE((regions.contentHeight == 300.0F - metrics.toolbarHeight - metrics.footerHeight));
        REQUIRE((regions.footerOrigin.y == 20.0F + 300.0F - metrics.footerHeight));
        REQUIRE((regions.leftRailHeight == regions.contentHeight));
    }

    TEST_CASE("Global dock search yields space to trailing controls at every width", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        REQUIRE((ResolveGlobalDockSearchWidth(800.0F, 500.0F) == 280.0F));
        REQUIRE((ResolveGlobalDockSearchWidth(400.0F, 500.0F) == 1.0F));
        REQUIRE((ResolveGlobalDockSearchWidth(800.0F, -20.0F) == 780.0F));
    }

    TEST_CASE("Editor wheel smoothing preserves precision and bounds discrete input per frame", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        EditorScrollSmoother smoother;
        smoother.Queue(0.0F, 1.0F);
        const EditorScrollDelta first = smoother.Consume(1.0F / 60.0F);
        REQUIRE((first.vertical > 0.0F));
        REQUIRE((first.vertical < 0.2F));

        float total = first.vertical;
        for (int frame = 0; frame < 7; ++frame)
            total += smoother.Consume(1.0F / 60.0F).vertical;
        REQUIRE((std::abs(total - 1.0F) < 0.001F));
        REQUIRE(smoother.Consume(1.0F / 60.0F).IsEmpty());

        smoother.Queue(0.05F, -0.05F);
        const EditorScrollDelta precise = smoother.Consume(1.0F / 60.0F);
        REQUIRE((std::abs(precise.horizontal - 0.05F) < 0.001F));
        REQUIRE((std::abs(precise.vertical + 0.05F) < 0.001F));

        smoother.Queue(0.0F, 1.0F);
        static_cast<void>(smoother.Consume(1.0F / 60.0F));
        smoother.Queue(0.0F, -1.0F);
        REQUIRE((smoother.Consume(1.0F / 60.0F).vertical < 0.0F));
    }

    TEST_CASE("Default workspace composes module-provided global dock panes", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        int factoryCalls = 0;
        const std::array<GlobalDockPaneFactory, 1> factories{
            [&factoryCalls] {
            ++factoryCalls;
            return std::make_unique<TestGlobalDockPane>();
        },
        };
        WorkspacePanelRegistry registry;
        RegisterDefaultWorkspacePanels(registry, factories);
        REQUIRE((factoryCalls == 1));

        const auto &panels = registry.GetAllPanels();
        const auto globalDock = std::ranges::find_if(panels, [](const std::shared_ptr<IWorkspacePanel> &panel) {
            return panel->GetId() == "horo.global_dock";
        });
        REQUIRE((globalDock != panels.end()));
        auto concreteDock = std::dynamic_pointer_cast<GlobalDockPanel>(*globalDock);
        REQUIRE(concreteDock);
        REQUIRE(concreteDock->ActivatePane("test.global_dock.custom"));
    }

    TEST_CASE("Build output status presentation maps every typed result and severity", "[unit][editor][gui]") {
        using namespace Horo;
        using Pane = Horo::Editor::GlobalDockBuildOutputPane;
        using ColorRole = Pane::BuildStatusColorRole;
        using Presentation = Pane::BuildStatusPresentation;

        struct StatusExpectation {
            BuildOutputRecord record;
            Presentation presentation;
        };

        const std::array expectations{
            StatusExpectation{BuildOutputRecord{.result = BuildOutputResult::Succeeded},
                              Presentation{ColorRole::Positive, "OK", "workspace.global_dock.build_output.row_status.succeeded"}},
            StatusExpectation{BuildOutputRecord{.result = BuildOutputResult::Failed},
                              Presentation{ColorRole::Error, "FAILED", "workspace.global_dock.build_output.row_status.failed"}},
            StatusExpectation{BuildOutputRecord{.result = BuildOutputResult::Cached},
                              Presentation{ColorRole::Muted, "CACHED", "workspace.global_dock.build_output.row_status.cached"}},
            StatusExpectation{BuildOutputRecord{.result = BuildOutputResult::Cancelled},
                              Presentation{ColorRole::Warning, "CANCELLED", "workspace.global_dock.build_output.row_status.cancelled"}},
            StatusExpectation{BuildOutputRecord{.result = BuildOutputResult::TimedOut},
                              Presentation{ColorRole::Error, "TIMED OUT", "workspace.global_dock.build_output.row_status.timed_out"}},
            StatusExpectation{BuildOutputRecord{.severity = DiagnosticSeverity::Fatal},
                              Presentation{ColorRole::Error, "FATAL", "workspace.global_dock.build_output.row_status.fatal"}},
            StatusExpectation{BuildOutputRecord{.severity = DiagnosticSeverity::Error},
                              Presentation{ColorRole::Error, "ERROR", "workspace.global_dock.build_output.row_status.failed"}},
            StatusExpectation{BuildOutputRecord{.severity = DiagnosticSeverity::Warning},
                              Presentation{ColorRole::Warning, "WARNING", "workspace.global_dock.build_output.row_status.warning"}},
            StatusExpectation{BuildOutputRecord{.severity = DiagnosticSeverity::Note},
                              Presentation{ColorRole::Accent, "INFO", "workspace.global_dock.build_output.row_status.info"}},
            StatusExpectation{BuildOutputRecord{.severity = static_cast<DiagnosticSeverity>(255)},
                              Presentation{ColorRole::Default, "INFO", "workspace.global_dock.build_output.row_status.info"}},
        };

        for (const StatusExpectation &expectation : expectations)
            REQUIRE((Pane::ProjectStatusPresentation(expectation.record) == expectation.presentation));
    }

    TEST_CASE("Build and operation projections use typed status and case-insensitive text", "[unit][editor][gui]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::array buildRecords{
            BuildOutputRecord{.result = BuildOutputResult::Succeeded,
                              .stage = "Compile",
                              .code = DiagnosticCode{"shader.compile.succeeded"},
                              .message = "Shader complete"},
            BuildOutputRecord{.severity = DiagnosticSeverity::Error,
                              .stage = "Validate",
                              .code = DiagnosticCode{"shader.validation.syntax"},
                              .message = "Invalid syntax",
                              .source = DiagnosticSourceLocation{.absolutePath = "/project/assets/material.glsl", .line = 8}},
            BuildOutputRecord{.result = BuildOutputResult::Cached,
                              .stage = "Reuse",
                              .code = DiagnosticCode{"mesh.cache.hit"},
                              .message = "Reused mesh"},
            BuildOutputRecord{.severity = DiagnosticSeverity::Warning,
                              .stage = "Compile",
                              .code = DiagnosticCode{"shader.compile.deprecated"},
                              .message = "Deprecated syntax"},
            BuildOutputRecord{.severity = DiagnosticSeverity::Fatal,
                              .stage = "Validate",
                              .code = DiagnosticCode{"shader.validation.abort"},
                              .message = "Compiler process cannot continue"},
            BuildOutputRecord{.result = BuildOutputResult::Failed,
                              .stage = "Link",
                              .code = DiagnosticCode{"gameplay.link.terminal"},
                              .message = "Linker stopped"},
            BuildOutputRecord{.result = BuildOutputResult::Cancelled,
                              .stage = "Generate",
                              .code = DiagnosticCode{"gameplay.generate.cancel"},
                              .message = "Generation stopped"},
            BuildOutputRecord{.result = BuildOutputResult::TimedOut,
                              .stage = "Package",
                              .code = DiagnosticCode{"gameplay.package.timeout"},
                              .message = "Packaging stopped"},
            BuildOutputRecord{.severity = DiagnosticSeverity::Note,
                              .stage = "Resolve",
                              .code = DiagnosticCode{"gameplay.resolve.detail"},
                              .message = "Dependency resolved"},
        };
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Failed, "MATERIAL") ==
                 std::vector<std::size_t>{1}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Cached, {}) ==
                 std::vector<std::size_t>{2}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Failed, "deprecated")
                     .empty()));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All,
                                                           "shader.compile.deprecated") == std::vector<std::size_t>{3}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Failed, "FATAL") ==
                 std::vector<std::size_t>{4}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Ok, {}) ==
                 std::vector<std::size_t>{0, 8}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Failed, {}) ==
                 std::vector<std::size_t>{1, 4, 5, 6, 7}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Errors, {}) ==
                 std::vector<std::size_t>{1, 4, 5, 7}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::Warning, {}) ==
                 std::vector<std::size_t>{3, 6}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "OK") ==
                 std::vector<std::size_t>{0}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "ERROR") ==
                 std::vector<std::size_t>{1}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "CACHED") ==
                 std::vector<std::size_t>{2}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "WARNING") ==
                 std::vector<std::size_t>{3}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "FAILED") ==
                 std::vector<std::size_t>{5}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "CANCELLED") ==
                 std::vector<std::size_t>{6}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "TIMED OUT") ==
                 std::vector<std::size_t>{7}));
        REQUIRE((GlobalDockBuildOutputPane::ProjectRecords(buildRecords, GlobalDockBuildOutputPane::StatusFilter::All, "INFO") ==
                 std::vector<std::size_t>{8}));

        const std::array operations{
            OperationRecord{.id = 1, .kind = OperationKind::Import, .state = OperationState::Running, .title = "Import assets"},
            OperationRecord{.id = 2,
                            .kind = OperationKind::Validation,
                            .state = OperationState::Failed,
                            .title = "Validate project",
                            .message = "Missing source"},
        };
        REQUIRE((GlobalDockOperationsPane::ProjectRecords(operations, "FAILED") == std::vector<std::size_t>{1}));
        REQUIRE((GlobalDockOperationsPane::ProjectRecords(operations, "import") == std::vector<std::size_t>{0}));
    }

    TEST_CASE("Content browser exposes absolute folders assets and breadcrumbs", "[unit][editor][gui]") {
        using namespace Horo;
        using namespace Horo::Assets;
        using namespace Horo::Editor;

        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-content-browser-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path assetRoot = projectRoot / "assets";
        const std::filesystem::path shotguns = assetRoot / "Props/Guns/Shotguns";
        std::filesystem::create_directories(shotguns);
        {
            std::ofstream payload(assetRoot / "root.horoasset", std::ios::binary);
            payload << "asset";
        }

        AssetRegistry registry;
        const auto report = registry.Publish({
            AssetRecord{
                .id = AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value(),
                .type = AssetTypeId::Parse("core.mesh").Value(),
                .sourcePath = ProjectPath::Parse("assets/root.horoasset").Value(),
                .metadataPath = ProjectPath::Parse("assets/root.horoasset.horo").Value(),
            },
        });
        REQUIRE((report.status == AssetRegistryBuildStatus::Complete));

        const auto root = BuildContentBrowserDirectory(projectRoot, {}, registry.Snapshot());
        REQUIRE((std::filesystem::path{root.absoluteRootPath}.is_absolute()));
        REQUIRE((root.absoluteCurrentPath == root.absoluteRootPath));
        REQUIRE((root.entries.size() == 2));
        REQUIRE((root.entries[0].kind == ContentBrowserEntryKind::Directory));
        REQUIRE((root.entries[0].displayName == "Props"));
        REQUIRE((std::filesystem::path{root.entries[0].absolutePath}.is_absolute()));
        REQUIRE((root.entries[0].containedItemCount == 1));
        REQUIRE((root.entries[1].kind == ContentBrowserEntryKind::Asset));
        REQUIRE((root.entries[1].displayName == "root"));
        REQUIRE((root.entries[1].assetType == "core.mesh"));

        const auto nested = BuildContentBrowserDirectory(projectRoot, shotguns, registry.Snapshot());
        REQUIRE((nested.breadcrumbs.size() == 4));
        REQUIRE((nested.breadcrumbs[0].label == "assets"));
        REQUIRE((nested.breadcrumbs[1].label == "Props"));
        REQUIRE((nested.breadcrumbs[2].label == "Guns"));
        REQUIRE((nested.breadcrumbs[3].label == "Shotguns"));
        for (const auto &breadcrumb : nested.breadcrumbs) {
            REQUIRE((std::filesystem::path{breadcrumb.absolutePath}.is_absolute()));
            REQUIRE((breadcrumb.absolutePath.find("..") == std::string::npos));
        }
        REQUIRE((!IsContentBrowserDirectoryTargetAllowed(assetRoot, "Props")));
        REQUIRE((!IsContentBrowserDirectoryTargetAllowed(assetRoot, projectRoot)));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    TEST_CASE("Content browser search type filter and sort project immutable entry indices", "[unit][editor][gui]") {
        using namespace Horo::Editor;

        const ContentBrowserDirectory directory{
            .entries =
                {
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Directory,
                        .absolutePath = "/project/assets/Props",
                        .displayName = "Props",
                    },
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Asset,
                        .absolutePath = "/project/assets/crate.horoasset",
                        .displayName = "Crate",
                        .assetType = "core.mesh",
                    },
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Asset,
                        .absolutePath = "/project/assets/wall.horoasset",
                        .displayName = "Wall",
                        .assetType = "core.texture",
                    },
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Asset,
                        .absolutePath = "/project/assets/hero.horoasset",
                        .displayName = "hero",
                        .assetType = "core.mesh",
                    },
                },
            .readable = true,
        };

        const auto searched = ProjectContentBrowserEntries(directory, {.name = "wALL"});
        REQUIRE((searched == std::vector<std::size_t>{2}));

        const auto meshes = ProjectContentBrowserEntries(directory, {.assetType = "core.mesh"});
        REQUIRE((meshes == std::vector<std::size_t>{0, 1, 3}));

        const auto byDescendingType = ProjectContentBrowserEntries(directory, {
                                                                                  .sortField = ContentBrowserSortField::Type,
                                                                                  .sortDirection = ContentBrowserSortDirection::Descending,
                                                                              });
        REQUIRE((byDescendingType == std::vector<std::size_t>{0, 2, 3, 1}));
    }

    TEST_CASE("Content browser uses the mesh fallback for topology-free legacy assets", "[unit][editor][gui]") {
        using namespace Horo;
        using namespace Horo::Editor;

        const std::filesystem::path projectRoot =
            std::filesystem::temp_directory_path() /
            ("horo-content-browser-legacy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path assetRoot = projectRoot / "assets/Meshes";
        std::filesystem::create_directories(assetRoot);
        const std::filesystem::path assetPath = assetRoot / "legacy.horoasset";

        std::vector<std::uint8_t> payload;
        const auto writeU32 = [&payload](const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                payload.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        };
        const auto writeFloat = [&writeU32](const float value) {
            writeU32(std::bit_cast<std::uint32_t>(value));
        };
        writeU32(Assets::MeshEditorPayloadSchemaVersion);
        writeU32(2);
        writeU32(1);
        for (const float bound : {-1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F})
            writeFloat(bound);
        writeU32(24);
        writeU32(0);
        writeU32(0);
        for (const float component : {-1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F})
            writeFloat(component);
        {
            std::ofstream output(assetPath, std::ios::binary);
            output.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }
        {
            std::ofstream metadata(assetPath.string() + ".meta");
            metadata << R"({"sourceFile":"/tmp/legacy.obj","type":"core.mesh"})";
        }

        Assets::AssetImporterCatalog importerCatalog;
        REQUIRE((Assets::RegisterObjMeshImporter(importerCatalog).HasValue()));
        auto publishedCatalog = importerCatalog.Publish();
        REQUIRE(publishedCatalog.HasValue());
        const ContentBrowserDirectory directory = BuildContentBrowserDirectory(projectRoot, assetRoot, {}, publishedCatalog.Value().get());
        REQUIRE((directory.readable));
        REQUIRE((directory.entries.size() == 1));
        REQUIRE((directory.entries[0].displayName == "legacy"));
        REQUIRE((directory.entries[0].assetType == "core.mesh"));
        REQUIRE((!directory.entries[0].registered));
        REQUIRE((std::filesystem::path{directory.entries[0].absoluteMetadataPath}.is_absolute()));
        REQUIRE((directory.entries[0].importerContributionId == "horo.asset-importer.obj-mesh"));
        REQUIRE((directory.entries[0].importerModuleId == "horo.builtin.assets.importer.obj"));
        REQUIRE((directory.entries[0].importerModuleVersion == "1.0.0"));
        REQUIRE((directory.entries[0].previewFallback == Assets::AssetPreviewFallback::Mesh));
        REQUIRE((!directory.entries[0].previewImage.IsValid()));
        REQUIRE((!directory.entries[0].meshPreviewPoints.empty()));

        std::error_code cleanupError;
        std::filesystem::remove_all(projectRoot, cleanupError);
    }

    Horo::Editor::EditorWorkspaceViewCommandData RenderAtWidth(
        const float width, const char *windowId, Horo::Editor::GlobalDockPanel &panel, const Horo::Editor::EditorGuiContext &context,
        const Horo::Editor::ContentBrowserLoadState loadState = Horo::Editor::ContentBrowserLoadState::Ready) {
        using namespace Horo::Editor;

        EditorWorkspaceViewModel viewModel;
        viewModel.contentBrowser = ContentBrowserDirectory{
            .absoluteRootPath = "/tmp/HoroProject/assets",
            .absoluteCurrentPath = "/tmp/HoroProject/assets/Props",
            .breadcrumbs =
                {
                    ContentBrowserBreadcrumb{.label = "assets", .absolutePath = "/tmp/HoroProject/assets"},
                    ContentBrowserBreadcrumb{.label = "Props", .absolutePath = "/tmp/HoroProject/assets/Props"},
                },
            .entries =
                {
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Directory,
                        .absolutePath = "/tmp/HoroProject/assets/Props/Guns",
                        .displayName = "Guns",
                    },
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Asset,
                        .absolutePath = "/tmp/HoroProject/assets/Props/crate.horoasset",
                        .displayName = "crate.horoasset",
                        .assetId = "00112233-4455-6677-8899-aabbccddeeff",
                        .assetType = "core.mesh",
                    },
                    ContentBrowserEntry{
                        .kind = ContentBrowserEntryKind::Asset,
                        .absolutePath = "/tmp/HoroProject/assets/Props/wall.horoasset",
                        .displayName = "wall.horoasset",
                        .assetId = "10112233-4455-6677-8899-aabbccddeeff",
                        .assetType = "core.texture",
                    },
                },
            .readable = true,
            .loadState = loadState,
        };
        EditorWorkspaceViewCommandData command;
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width + 20.0F, 260.0F));
        ImGui::Begin(windowId, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        panel.DrawPanel(ImGui::GetCursorScreenPos(), ImVec2(width, 220.0F), viewModel, command, context);
        ImGui::End();
        return command;
    }
}  // namespace

TEST_CASE("Content browser renders responsive layouts and every dock tab", "[unit][editor][gui]") {
    using namespace Horo;
    using namespace Horo::Editor;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0F, 720.0F);
    io.DeltaTime = 1.0F / 60.0F;
    io.Fonts->AddFontDefault();
    static_cast<void>(io.Fonts->Build());

    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    TestLocalization localization;
    ImFont *defaultFont = io.Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = defaultFont, .sansCompact = defaultFont, .sansEmphasis = defaultFont};
    const ThemeContext theme{.fonts = fonts};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{.engineEvents = engineEvents,
                                   .editorEvents = editorEvents,
                                   .localization = localization,
                                   .theme = theme,
                                   .settings = settings};
    GlobalDockPanel panel;

    ImGui::NewFrame();
    RenderAtWidth(600.0F, "ContentBrowserWide", panel, context);
    ImGui::Render();

    ImGui::NewFrame();
    RenderAtWidth(260.0F, "ContentBrowserNarrow", panel, context);
    ImGui::Render();

    ImGui::NewFrame();
    RenderAtWidth(600.0F, "ContentBrowserLoading", panel, context, ContentBrowserLoadState::Loading);
    ImGui::Render();

    ImGui::NewFrame();
    RenderAtWidth(600.0F, "ContentBrowserError", panel, context, ContentBrowserLoadState::Error);
    ImGui::Render();

    for (const GlobalDockTab tab : DefaultGlobalDockTabs()) {
        GlobalDockPanel tabPanel{tab};
        ImGui::NewFrame();
        RenderAtWidth(900.0F, "GlobalDockTabMatrix", tabPanel, context);
        ImGui::Render();
        ImGui::NewFrame();
        RenderAtWidth(260.0F, "GlobalDockNarrowTabMatrix", tabPanel, context);
        ImGui::Render();
        REQUIRE((tabPanel.ActiveTab() == tab));
    }

    Log::StructuredLogStore logStore{8};
    for (std::size_t level = 0; level < 5; ++level) {
        logStore.Append(Log::StructuredLogRecord{
            .sequence = level + 1U,
            .timestampUtc = std::chrono::system_clock::now(),
            .level = static_cast<Log::Level>(level),
            .category = "editor.console.test",
            .message = "Live structured log row",
        });
    }
    GlobalDockPanel liveConsole{GlobalDockTab::Console};
    PanelContext panelContext{.dataBus = editorEvents, .logQuery = &logStore};
    liveConsole.OnAttach(panelContext);
    ImGui::NewFrame();
    RenderAtWidth(900.0F, "LiveConsoleRows", liveConsole, context);
    ImGui::Render();
    liveConsole.OnDetach();

    BuildOutputStore buildOutputStore{16};
    const std::array renderedBuildRecords{
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .result = BuildOutputResult::Succeeded,
                          .stage = "compile",
                          .message = "Build succeeded"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .result = BuildOutputResult::Failed,
                          .stage = "link",
                          .message = "Build failed"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .result = BuildOutputResult::Cached,
                          .stage = "cook",
                          .message = "Cache hit"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .result = BuildOutputResult::Cancelled,
                          .stage = "package",
                          .message = "Build cancelled"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .result = BuildOutputResult::TimedOut,
                          .stage = "package",
                          .message = "Build timed out"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .severity = DiagnosticSeverity::Error,
                          .stage = "validate",
                          .message = "Validation error"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .severity = DiagnosticSeverity::Warning,
                          .stage = "compile",
                          .message = "Compiler warning"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .severity = DiagnosticSeverity::Note,
                          .stage = "resolve",
                          .message = "Resolution note"},
        BuildOutputRecord{.timestampUtc = std::chrono::system_clock::now(),
                          .severity = DiagnosticSeverity::Fatal,
                          .stage = "compile",
                          .code = DiagnosticCode{"shader.compile.abort"},
                          .message = "Unable to compile shader",
                          .source = DiagnosticSourceLocation{.absolutePath = "/tmp/HoroProject/assets/shader.glsl", .line = 12}},
    };
    for (const BuildOutputRecord &record : renderedBuildRecords)
        buildOutputStore.Append(record);
    OperationStore operationStore{8, 8};
    const auto operationId = operationStore.Begin({.kind = OperationKind::Cook,
                                                   .title = "Cook assets",
                                                   .phase = "cook",
                                                   .message = "4 of 8",
                                                   .progress = 0.5F,
                                                   .cancellable = true,
                                                   .requestCancel = [] {
    }});
    REQUIRE(operationId.has_value());
    REQUIRE(
        operationStore.Update(*operationId, {.state = OperationState::Running, .phase = "cook", .message = "4 of 8", .progress = 0.5F}));

    PanelContext activityContext{.dataBus = editorEvents,
                                 .buildOutputQuery = &buildOutputStore,
                                 .operationQuery = &operationStore,
                                 .operationControl = &operationStore};
    BuildOutputStore clickableBuildOutputStore{4};
    clickableBuildOutputStore.Append(BuildOutputRecord{
        .timestampUtc = std::chrono::system_clock::now(),
        .severity = DiagnosticSeverity::Error,
        .stage = "compile",
        .message = "Clickable diagnostic",
        .source = DiagnosticSourceLocation{.absolutePath = "/tmp/HoroProject/assets/shader.glsl", .line = 12, .column = 3},
    });
    PanelContext clickableActivityContext{.dataBus = editorEvents, .buildOutputQuery = &clickableBuildOutputStore};
    GlobalDockPanel clickableBuild{GlobalDockTab::BuildOutput};
    clickableBuild.OnAttach(clickableActivityContext);
    GlobalDockPanel liveBuild{GlobalDockTab::BuildOutput};
    liveBuild.OnAttach(activityContext);
    const GlobalDockPaneMetrics buildMetrics = ResolveGlobalDockPaneMetrics();
    const float buildRowClickY = ImGui::GetStyle().WindowPadding.y + 36.0F + buildMetrics.toolbarHeight + buildMetrics.tableHeaderHeight +
                                 buildMetrics.tableRowHeight * 0.5F;
    io.AddMousePosEvent(80.0F, buildRowClickY);
    ImGui::NewFrame();
    RenderAtWidth(900.0F, "ClickableBuildRow", clickableBuild, context);
    ImGui::Render();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ImGui::NewFrame();
    RenderAtWidth(900.0F, "ClickableBuildRow", clickableBuild, context);
    ImGui::Render();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    ImGui::NewFrame();
    const EditorWorkspaceViewCommandData diagnosticCommand = RenderAtWidth(900.0F, "ClickableBuildRow", clickableBuild, context);
    ImGui::Render();
    REQUIRE((diagnosticCommand.command == EditorWorkspaceViewCommand::OpenDiagnosticSource));
    REQUIRE(diagnosticCommand.diagnosticSource.has_value());
    REQUIRE((diagnosticCommand.diagnosticSource->absolutePath == "/tmp/HoroProject/assets/shader.glsl"));
    REQUIRE((diagnosticCommand.diagnosticSource->line == 12U));
    REQUIRE((diagnosticCommand.diagnosticSource->column == 3U));
    clickableBuild.OnDetach();
    ImGui::NewFrame();
    RenderAtWidth(900.0F, "LiveBuildRows", liveBuild, context);
    ImGui::Render();
    ImGui::NewFrame();
    RenderAtWidth(260.0F, "LiveBuildRowsNarrow", liveBuild, context);
    ImGui::Render();
    liveBuild.OnDetach();

    GlobalDockPanel liveOperations{GlobalDockTab::Operations};
    liveOperations.OnAttach(activityContext);
    ImGui::NewFrame();
    RenderAtWidth(900.0F, "LiveOperationRows", liveOperations, context);
    ImGui::Render();
    ImGui::NewFrame();
    RenderAtWidth(260.0F, "LiveOperationRowsNarrow", liveOperations, context);
    ImGui::Render();
    liveOperations.OnDetach();

    ImGui::DestroyContext();
}
