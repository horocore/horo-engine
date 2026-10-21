#include "Horo/Editor/NativeAssetRegistrySource.h"
#include "Horo/Editor/WorkspaceReadSources.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"
#include "PartitionedWorldResidencyTestSupport.h"
#include "WorldCompositionSourceTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor::WorldCompositionTests {
    namespace {
        struct WorldProject final {
            Horo::Tests::OwnedTestDirectory directory{"world composition"};
            Runtime::RuntimeSceneService runtime;
            std::unique_ptr<EditorWorkspaceController> controller;

            WorldProject() {
                {
                    std::ofstream file{Path()};
                    file << WorldSource();
                }
                const auto owner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000009").Value();
                const WorkspaceReadSourceFactory factory{[owner](const std::filesystem::path &root) {
                    auto registry = CreateNativeAssetRegistrySource(root, owner.ToString(), 1);
                    auto files = Platform::CreateNativeProjectReadFiles(root, owner.ToString(), 1);
                    REQUIRE(registry.HasValue());
                    REQUIRE(files.HasValue());
                    return Result<WorkspaceReadSources>::Success({owner, registry.Value(), files.Value()});
                }};
                auto captured = CaptureWorkspaceReadSources(&factory, directory.Path());
                REQUIRE(captured.HasValue());
                const auto &sources = captured.Value();
                controller =
                    std::make_unique<EditorWorkspaceController>(directory.Path(), runtime, Assets::AssetRegistrySnapshot{},
                                                                EditorWorkspaceDependencies{.assetRegistrySource = sources.registry,
                                                                                            .worldSourceFiles = sources.world,
                                                                                            .documentSessionOwner = sources.owner});
                REQUIRE(controller->OpenWorldCompositionSource("world ü space.horoworld").HasValue());
            }

            void CaptureFrame() {
                controller->RefreshWorldSourceProjection();
                controller->RefreshWorldDocumentProjection();
            }

            std::filesystem::path Path() const {
                return directory.Path() / std::filesystem::path{u8"world ü space.horoworld"};
            }
        };

        EditorWorkspaceViewCommandData CreateSource(const WorldViewportBinding &binding) {
            PartitionedWorldSource source{.shape = WorldStreaming::StreamingSourceSphere{binding.globalOrigin, 1000},
                                          .priority = WorldStreaming::StreamingSourcePriority::Create(1).Value()};
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::WorldSourceTool;
            command.worldSource = WorldSourceToolCommand{.action = WorldSourceToolAction::Create,
                                                         .edit = {.expected = binding.snapshot->fence, .value = source},
                                                         .globalOrigin = binding.globalOrigin,
                                                         .viewRevision = binding.viewRevision};
            return command;
        }
    }  // namespace

    TEST_CASE("Actual world source route binds canonical owner and persistent tab atomically", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        const auto current = controller.WorldDocuments().Snapshot().Value();
        REQUIRE(controller.ViewModel().worldDocument == current);
        REQUIRE(controller.ViewModel().worldBrowser);
        CHECK(controller.ViewModel().worldBrowser->cells.size() == 3);
        CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
        CHECK(controller.ViewModel().worldDocumentSource == "world ü space.horoworld");
        CHECK(controller.ViewModel().worldViewport.snapshot == current);
        CHECK(controller.ViewModel().worldViewport.globalOrigin == current->document.settings.grid.Origin());
        CHECK(controller.ViewModel().worldViewport.viewRevision != 0);
    }

    TEST_CASE("World source open command routes payload and preserves publication on missing input",
              "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        const auto original = controller.WorldDocuments().Snapshot().Value();
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenWorldCompositionSource;
        open.worldDocumentFence = original->fence;
        SECTION("valid payload") {
            open.stringPayload = "world ü space.horoworld";
            controller.ProcessCommand(open);
            const auto reopened = controller.WorldDocuments().Snapshot().Value();
            CHECK(reopened->fence.generation != original->fence.generation);
            CHECK(controller.ViewModel().worldDocument == reopened);
            CHECK(controller.ViewModel().worldViewport.snapshot == reopened);
            REQUIRE(controller.ViewModel().worldBrowser);
            CHECK(controller.ViewModel().worldBrowser->source == reopened);
            CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
        }
        SECTION("missing payload") {
            controller.ProcessCommand(open);
            REQUIRE(controller.ViewModel().worldDocumentError);
            CHECK(controller.ViewModel().worldDocumentError->code.Value() == WorldDocumentErrors::Invalid.code.Value());
            CHECK(controller.WorldDocuments().Snapshot().Value() == original);
            CHECK(controller.ViewModel().worldViewport.snapshot == original);
        }
    }

    TEST_CASE("Actual viewport producer and dirty guards retain the same opened world", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        controller.ProcessCommand(CreateSource(controller.ViewModel().worldViewport));
        const auto edited = controller.WorldDocuments().Snapshot().Value();
        REQUIRE(edited->document.sources.size() == 1);
        CHECK(controller.ViewModel().worldDocument == edited);
        CHECK(controller.ViewModel().worldViewport.snapshot == edited);
        CHECK(controller.ViewModel().worldBrowser->source == edited);
        REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld", edited->fence).HasError());
        CHECK(controller.WorldDocuments().Snapshot().Value() == edited);
        EditorWorkspaceViewCommandData close;
        close.command = EditorWorkspaceViewCommand::CloseWorldComposition;
        close.worldDocumentFence = edited->fence;
        controller.ProcessCommand(close);
        REQUIRE(controller.ViewModel().worldDocumentError);
        CHECK(controller.ViewModel().worldDocumentError->code.Value() == WorldDocumentErrors::Dirty.code.Value());
        CHECK(controller.WorldDocuments().Snapshot().Value() == edited);
        REQUIRE(controller.WorldDocuments().Shutdown(WorldDocumentDisposition::Discard).HasValue());
        controller.RefreshWorldDocumentProjection();
        CHECK_FALSE(controller.ViewModel().worldDocument);
        CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
    }

    TEST_CASE("Content browser source navigation rebinds the same canonical world owner", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        const auto current = controller.WorldDocuments().Snapshot().Value();
        EditorWorkspaceViewCommandData sourceOpen;
        sourceOpen.command = EditorWorkspaceViewCommand::OpenSourceFile;
        sourceOpen.sourceOpenRequest = SourceOpenRequest{.path = project.Path()};
        controller.ProcessCommand(sourceOpen);
        const auto reopened = controller.WorldDocuments().Snapshot().Value();
        CHECK(reopened->fence.generation != current->fence.generation);
        CHECK(controller.ViewModel().worldViewport.snapshot == reopened);
        CHECK(controller.ViewModel().worldViewport.viewRevision > 1);
        CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
    }

    TEST_CASE("Failed malformed stale and unsafe world source replacement preserve owner and focus",
              "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        const auto current = controller.WorldDocuments().Snapshot().Value();
        SECTION("malformed") {
            {
                std::ofstream file{project.Path()};
                file << "{invalid";
            }
            REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld", current->fence).HasError());
        }
        SECTION("stale") {
            REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld").HasError());
        }
        SECTION("containment") {
            REQUIRE(controller.OpenWorldCompositionSource("../world.horoworld", current->fence).HasError());
        }
        SECTION("missing") {
            REQUIRE(controller.OpenWorldCompositionSource("missing.horoworld", current->fence).HasError());
        }
        CHECK(controller.WorldDocuments().Snapshot().Value() == current);
        CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
    }

    TEST_CASE("World browser filter and close commands enforce exact revision fences", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        const auto current = controller.WorldDocuments().Snapshot().Value();
        EditorWorkspaceViewCommandData query;
        query.command = EditorWorkspaceViewCommand::QueryWorldComposition;
        query.worldDocumentFence = current->fence;
        query.worldBrowserQuery = WorldBrowserQuery{.search = "missing"};
        controller.ProcessCommand(query);
        REQUIRE(controller.ViewModel().worldBrowser->cells.empty());
        query.worldBrowserQuery = WorldBrowserQuery{.maximumCells = 1};
        controller.ProcessCommand(query);
        REQUIRE(controller.ViewModel().worldDocumentError);
        REQUIRE(controller.ViewModel().worldBrowserQuery.search == "missing");
        const auto replaced = controller.WorldDocuments().Replace(current->document, current->fence).Value();
        query.command = EditorWorkspaceViewCommand::CloseWorldComposition;
        controller.ProcessCommand(query);
        REQUIRE(controller.WorldDocuments().Snapshot().Value() == replaced);
        query.worldDocumentFence = replaced->fence;
        controller.ProcessCommand(query);
        CHECK_FALSE(controller.ViewModel().worldDocument);
        CHECK_FALSE(controller.ViewModel().worldBrowser);
        CHECK(current->document.pages.size() == 1);
    }

    TEST_CASE("Real page payload residency updates the same browser and viewport frame", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        auto &world = controller.WorldDocuments();
        const auto initial = world.Snapshot().Value();
        const auto &page = initial->document.pages.front().head;
        REQUIRE(world.LoadPage(page, WorldResidencyTests::Source(), initial->fence).HasValue());
        project.CaptureFrame();
        const auto loaded = world.Snapshot().Value();
        REQUIRE(loaded->loadedPages.front().content.Objects().size() == 1);
        REQUIRE(controller.ViewModel().worldBrowser->cells.size() == 3);
        for (const auto &cell : controller.ViewModel().worldBrowser->cells)
            CHECK(cell.openObjects == 1);
        CHECK(controller.ViewModel().worldBrowser->source == loaded);
        CHECK(controller.ViewModel().worldViewport.snapshot == loaded);
        REQUIRE(world.UnloadPage(page.sourceAsset, loaded->fence).HasValue());
        project.CaptureFrame();
        const auto closed = world.Snapshot().Value();
        REQUIRE(controller.ViewModel().worldBrowser->cells.size() == 3);
        for (const auto &cell : controller.ViewModel().worldBrowser->cells)
            CHECK(cell.openObjects == 0);
        CHECK(controller.ViewModel().worldBrowser->source == closed);
        CHECK(controller.ViewModel().worldViewport.snapshot == closed);
        CHECK(closed->document.pages.size() == 1);
        CHECK_FALSE(closed->dirty);
        CHECK(loaded->loadedPages.front().content.Objects().size() == 1);
    }

    TEST_CASE("Page-aware selection prevents unload without changing browser publication", "[editor][world_browser][controller]") {
        WorldProject project;
        auto &controller = *project.controller;
        auto &world = controller.WorldDocuments();
        const auto initial = world.Snapshot().Value();
        const auto &page = initial->document.pages.front().head;
        REQUIRE(world.LoadPage(page, WorldResidencyTests::Source(), initial->fence).HasValue());
        const auto loaded = world.Snapshot().Value();
        const std::array addresses{WorldStreaming::WorldAuthoringObjectAddress{page.sourceAsset, 1}};
        const auto selected = controller.SelectWorldObjects(addresses, addresses.front(), loaded->fence);
        REQUIRE(selected.HasValue());
        project.CaptureFrame();
        WorldResidencyTests::ErrorIs(world.UnloadPage(page.sourceAsset, selected.Value()->fence), "world_document.conflict");
        project.CaptureFrame();
        CHECK(world.Snapshot().Value() == selected.Value());
        CHECK(controller.ViewModel().worldBrowser->source == selected.Value());
        CHECK(controller.ViewModel().worldViewport.snapshot == selected.Value());
        CHECK(controller.ViewModel().worldDocument->primary == addresses.front());
        const auto cleared = controller.SelectWorldObjects({}, std::nullopt, selected.Value()->fence);
        REQUIRE(cleared.HasValue());
        REQUIRE(world.UnloadPage(page.sourceAsset, cleared.Value()->fence).HasValue());
        project.CaptureFrame();
        CHECK(controller.ViewModel().worldDocument->selectedObjects.empty());
        CHECK(controller.ViewModel().worldDocument->loadedPages.empty());
    }
}  // namespace Horo::Editor::WorldCompositionTests
