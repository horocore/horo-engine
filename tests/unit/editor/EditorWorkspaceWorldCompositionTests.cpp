#include "Horo/Platform/NativeProjectReadFiles.h"
#include "OwnedTestDirectory.h"
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
                auto files = Platform::CreateNativeProjectReadFiles(directory.Path(), owner.ToString(), 1);
                REQUIRE(files.HasValue());
                controller = std::make_unique<EditorWorkspaceController>(directory.Path(), runtime, Assets::AssetRegistrySnapshot{},
                                                                         EditorWorkspaceDependencies{.worldSourceFiles = files.Value(),
                                                                                                     .documentSessionOwner = owner});
                REQUIRE(controller->OpenWorldCompositionSource("world ü space.horoworld").HasValue());
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
}  // namespace Horo::Editor::WorldCompositionTests
