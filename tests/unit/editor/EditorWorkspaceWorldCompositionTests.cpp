#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"
#include "WorldCompositionSourceTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace Horo::Editor::WorldCompositionTests {
    TEST_CASE("Actual world source route binds canonical owner and persistent tab atomically", "[editor][world_browser][controller]") {
        Horo::Tests::OwnedTestDirectory project{"world composition ü space"};
        const auto path = project.Path() / "world ü space.horoworld";
        {
            std::ofstream file{path};
            file << WorldSource();
        }
        Runtime::RuntimeSceneService runtime;
        const auto owner = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000009").Value();
        auto readFiles = Platform::CreateNativeProjectReadFiles(project.Path(), owner.ToString(), 1);
        REQUIRE(readFiles.HasValue());
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             EditorWorkspaceDependencies{.worldSourceFiles = readFiles.Value(),
                                                                         .documentSessionOwner = owner}};
        REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld").HasValue());
        const auto current = controller.WorldDocuments().Snapshot().Value();
        REQUIRE(controller.ViewModel().worldDocument == current);
        REQUIRE(controller.ViewModel().worldBrowser);
        CHECK(controller.ViewModel().worldBrowser->cells.size() == 3);
        CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
        CHECK(controller.ViewModel().worldDocumentSource == "world ü space.horoworld");
        CHECK(controller.ViewModel().worldViewport.snapshot == current);
        CHECK(controller.ViewModel().worldViewport.globalOrigin == current->document.settings.grid.Origin());
        CHECK(controller.ViewModel().worldViewport.viewRevision != 0);
        SECTION("actual viewport source producer commits into the same opened owner") {
            EditorWorkspaceViewCommandData create;
            create.command = EditorWorkspaceViewCommand::WorldSourceTool;
            const auto binding = controller.ViewModel().worldViewport;
            create.worldSource =
                WorldSourceToolCommand{.action = WorldSourceToolAction::Create,
                                       .edit = {.expected = current->fence,
                                                .value =
                                                    PartitionedWorldSource{.shape =
                                                                               WorldStreaming::StreamingSourceSphere{binding.globalOrigin,
                                                                                                                     1000},
                                                                           .priority =
                                                                               WorldStreaming::StreamingSourcePriority::Create(1).Value()}},
                                       .globalOrigin = binding.globalOrigin,
                                       .viewRevision = binding.viewRevision};
            controller.ProcessCommand(create);
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
            CHECK(controller.ViewModel().worldDocumentError->code == WorldDocumentErrors::Dirty.code);
            CHECK(controller.WorldDocuments().Snapshot().Value() == edited);
            REQUIRE(controller.WorldDocuments().Shutdown(WorldDocumentDisposition::Discard).HasValue());
            controller.RefreshWorldDocumentProjection();
            CHECK_FALSE(controller.ViewModel().worldDocument);
            CHECK_FALSE(controller.ViewModel().worldViewport.snapshot);
        }
        SECTION("content browser source-open uses the same owned world route") {
            EditorWorkspaceViewCommandData sourceOpen;
            sourceOpen.command = EditorWorkspaceViewCommand::OpenSourceFile;
            sourceOpen.sourceOpenRequest = SourceOpenRequest{.path = path};
            controller.ProcessCommand(sourceOpen);
            const auto reopened = controller.WorldDocuments().Snapshot().Value();
            CHECK(reopened->fence.generation != current->fence.generation);
            CHECK(controller.ViewModel().worldViewport.snapshot == reopened);
            CHECK(controller.ViewModel().worldViewport.viewRevision > 1);
            CHECK(controller.ViewModel().activeDocumentPanelId == "horo.world_composition");
        }
        SECTION("failed replacement preserves owner and tab") {
            {
                std::ofstream file{path};
                file << "{invalid";
            }
            REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld", current->fence).HasError());
            REQUIRE(controller.WorldDocuments().Snapshot().Value() == current);
        }
        SECTION("stale open command cannot replace current source") {
            REQUIRE(controller.OpenWorldCompositionSource("world ü space.horoworld").HasError());
            REQUIRE(controller.WorldDocuments().Snapshot().Value() == current);
        }
        SECTION("portable containment and missing file failures") {
            REQUIRE(controller.OpenWorldCompositionSource("../world.horoworld", current->fence).HasError());
            REQUIRE(controller.OpenWorldCompositionSource("missing.horoworld", current->fence).HasError());
            REQUIRE(controller.WorldDocuments().Snapshot().Value() == current);
        }
        SECTION("filter and close route preserve typed revision fencing") {
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
    }
}  // namespace Horo::Editor::WorldCompositionTests
