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
                                                                EditorWorkspaceDependencies{.world = {.files = sources.world},
                                                                                            .assetRegistrySource = sources.registry,
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
            controller.ProcessCommand(open);
            REQUIRE(controller.ViewModel().worldDocumentError);
            CHECK(controller.ViewModel().worldDocumentError->code.Value() == WorldDocumentErrors::Stale.code.Value());
            CHECK(controller.ViewModel().worldDocumentError->domain.Value() == WorldDocumentErrors::Stale.domain.Value());
            CHECK(controller.WorldDocuments().Snapshot().Value() == reopened);
            CHECK(controller.ViewModel().worldViewport.snapshot == reopened);
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

namespace Horo::Editor {
    TEST_CASE("World Composition bake command uses the application-owned real cell producer and shared diagnostic stores",
              "[editor][world_bake][controller]") {
        Horo::Tests::OwnedTestDirectory project{"world bake controller"};
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore diagnostics{32};
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             EditorWorkspaceDependencies{.jobs = &jobs,
                                                                         .worldBake = {.operations = &operations, .output = &diagnostics},
                                                                         .documentSessionOwner = WorldResidencyTests::Asset(9)}};
        auto &world = controller.WorldDocuments();
        REQUIRE(world.Open(WorldResidencyTests::Document()).HasValue());
        for (const auto &page : WorldResidencyTests::Document().pages)
            REQUIRE(world.LoadPage(page.head, WorldResidencyTests::Source(), WorldResidencyTests::Current(world)->fence).HasValue());
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::BakeWorldCells;
        command.worldDocumentFence = WorldResidencyTests::Current(world)->fence;
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().worldBakeOperation);
        auto marker = jobs.SubmitResult({}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(marker.HasValue());
        REQUIRE(marker.Value()
                    .Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(5000)})
                    .HasValue());
        controller.UpdateWorldBake();
        CHECK(controller.ViewModel().worldBakeCurrent);
        CHECK(controller.SetWorldBakeProject({}).HasError());
        CHECK(controller.ViewModel().worldBakeCurrent);
        CHECK_FALSE(controller.ViewModel().worldBakeOperation);
        const auto output = diagnostics.SnapshotIfChanged(0);
        REQUIRE(output);
        REQUIRE_FALSE(output->records.empty());
        CHECK(output->records.back().result == BuildOutputResult::Succeeded);
        const auto previous = WorldResidencyTests::Current(world);
        command.worldDocumentFence->revision = WorldDocumentRevision::Create(previous->fence.revision.Value() + 1).Value();
        controller.ProcessCommand(command);
        REQUIRE(controller.ViewModel().worldDocumentError);
        CHECK(controller.ViewModel().worldDocumentError->code.Value() == WorldDocumentErrors::Stale.code.Value());
        CHECK(controller.ViewModel().worldDocumentError->domain.Value() == WorldDocumentErrors::Stale.domain.Value());
        CHECK(WorldResidencyTests::Current(world) == previous);
        CHECK(controller.ViewModel().worldBakeCurrent);
    }
}  // namespace Horo::Editor

namespace Horo::Editor {
    TEST_CASE("World bake project replacement cannot reuse a source publication fence", "[editor][world_bake][controller]") {
        Horo::Tests::OwnedTestDirectory project{"world bake project fence"};
        JobSystem jobs{{.workerCount = 0}};
        OperationStore operations{8, 16};
        BuildOutputStore output{32};
        Runtime::RuntimeSceneService runtime;
        Application::WorldBakeProjectCapture capture;
        capture.fence = {WorldResidencyTests::Asset(12), 7, {}};
        capture.fence.schemas.bytes.front() = 1;
        capture.settings.bytes.front() = 2;
        EditorWorkspaceController controller{project.Path(),
                                             runtime,
                                             {},
                                             EditorWorkspaceDependencies{.jobs = &jobs,
                                                                         .worldBake = {.operations = &operations,
                                                                                       .output = &output,
                                                                                       .project = capture},
                                                                         .documentSessionOwner = WorldResidencyTests::Asset(9)}};
        auto &world = controller.WorldDocuments();
        REQUIRE(world.Open(WorldResidencyTests::Document()).HasValue());
        for (const auto &page : WorldResidencyTests::Document().pages)
            REQUIRE(world.LoadPage(page.head, WorldResidencyTests::Source(), WorldResidencyTests::Current(world)->fence).HasValue());
        const auto accepted = controller.StartWorldBake(Application::WorldBakeMode::Bake, world.Snapshot().Value()->fence);
        REQUIRE(accepted.HasValue());
        auto replacement = capture;
        ++replacement.settings.bytes.front();
        const auto rejected = controller.SetWorldBakeProject(replacement);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Application::WorldBakeErrors::Stale.code.Value());
        CHECK(rejected.ErrorValue().domain.Value() == Application::WorldBakeErrors::Stale.domain.Value());
        CHECK(controller.ViewModel().worldBakeOperation == accepted.Value());
        ++replacement.fence.revision;
        REQUIRE(controller.SetWorldBakeProject(replacement).HasValue());
        CHECK_FALSE(controller.ViewModel().worldBakeOperation);
        CHECK_FALSE(controller.ViewModel().worldBakeCurrent);
        CHECK_FALSE(output.SnapshotIfChanged(0));
    }
}  // namespace Horo::Editor

namespace Horo::Editor {
    TEST_CASE("World bake publication fences the registry owner before a presentation snapshot refresh",
              "[editor][world_bake][controller]") {
        Horo::Tests::OwnedTestDirectory project{"world bake registry fence"};
        JobSystem jobs{{.workerCount = 1}};
        OperationStore operations{8, 16};
        BuildOutputStore output{32};
        Assets::AssetRegistry registry;
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{project.Path(), runtime, registry.Snapshot(),
                                             EditorWorkspaceDependencies{.mutableAssetRegistry = &registry,
                                                                         .jobs = &jobs,
                                                                         .worldBake = {.operations = &operations, .output = &output},
                                                                         .documentSessionOwner = WorldResidencyTests::Asset(9)}};
        auto &world = controller.WorldDocuments();
        REQUIRE(world.Open(WorldResidencyTests::Document()).HasValue());
        for (const auto &page : WorldResidencyTests::Document().pages)
            REQUIRE(world.LoadPage(page.head, WorldResidencyTests::Source(), WorldResidencyTests::Current(world)->fence).HasValue());
        const auto accepted = controller.StartWorldBake(Application::WorldBakeMode::Bake, world.Snapshot().Value()->fence);
        REQUIRE(accepted.HasValue());
        auto marker = jobs.SubmitResult({}, [](const CancellationToken &) {
            return Result<void>::Success();
        });
        REQUIRE(marker.HasValue());
        REQUIRE(marker.Value()
                    .Wait({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = Duration::FromMilliseconds(5000)})
                    .HasValue());
        const auto publication = registry.Publish(std::vector<Assets::AssetRecord>{});
        REQUIRE(publication.status == Assets::AssetRegistryBuildStatus::Complete);
        controller.UpdateWorldBake();
        CHECK_FALSE(controller.ViewModel().worldBakeCurrent);
        CHECK_FALSE(output.SnapshotIfChanged(0));
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot);
        REQUIRE(snapshot->operations.size() == 1);
        CHECK(snapshot->operations.front().state == OperationState::Cancelled);
        REQUIRE(snapshot->operations.front().error);
        CHECK(snapshot->operations.front().error->code.Value() == Application::WorldBakeErrors::Stale.code.Value());
        CHECK(snapshot->operations.front().error->domain.Value() == Application::WorldBakeErrors::Stale.domain.Value());
    }
}  // namespace Horo::Editor

namespace Horo::Editor {
    namespace {
        /** @brief Creates actual native fixture readers while preserving their exact shared opening fence. */
        Result<WorkspaceReadSources> CaptureTestReaders(const std::filesystem::path &root, const Assets::AssetId owner) {
            auto registry = CreateNativeAssetRegistrySource(root, owner.ToString(), 1);
            if (registry.HasError())
                return Result<WorkspaceReadSources>::Failure(registry.ErrorValue());
            auto world = Platform::CreateNativeProjectReadFiles(root, owner.ToString(), 1);
            if (world.HasError())
                return Result<WorkspaceReadSources>::Failure(world.ErrorValue());
            return Result<WorkspaceReadSources>::Success({owner, std::move(registry).Value(), std::move(world).Value()});
        }
    }  // namespace

    TEST_CASE("Workspace reader bridge retains actual native root authority", "[editor][world_bake][native]") {
        Horo::Tests::OwnedTestDirectory project{"workspace native readers"};
        const auto owner = WorldResidencyTests::Asset(9);
        const WorkspaceReadSourceFactory factory{[owner](const std::filesystem::path &root) {
            return CaptureTestReaders(root, owner);
        }};
        REQUIRE(std::filesystem::create_directory(project.Path() / "Assets"));
        {
            std::ofstream file{project.Path() / "Assets/page scene.horo"};
            file << WorldResidencyTests::Source();
            REQUIRE(file.good());
        }
        auto sources = CaptureWorkspaceReadSources(&factory, project.Path());
        REQUIRE(sources.HasValue());
        CHECK(sources.Value().owner == owner);
        const Platform::ProjectReadContext context{.projectIdentity = owner.ToString(),
                                                   .projectGeneration = 1,
                                                   .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        const auto bytes = sources.Value().world->SourceText("Assets/page scene.horo", context, {});
        REQUIRE(bytes.HasValue());
        REQUIRE(bytes.Value().text);
        CHECK(*bytes.Value().text == WorldResidencyTests::Source());
        const auto inventory = sources.Value().registry->Capture();
        REQUIRE(inventory.HasValue());
        CHECK(inventory.Value().assetRoot == "Assets");
        REQUIRE(inventory.Value().entries.size() == 1);
        CHECK(inventory.Value().entries.front().path == "Assets/page scene.horo");
    }

}  // namespace Horo::Editor
