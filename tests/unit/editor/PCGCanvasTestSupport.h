#pragma once
#include "PCGDocumentTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor::PCGCanvasTestSupport {
    using namespace PCGDocumentTestSupport;

    /** @brief Publishes the real registered source fixture used by canvas command and lifecycle cases. */
    inline void PublishGraphAsset(Assets::AssetRegistry &registry) {
        const auto source = ProjectPath::Parse(Identity().key.source.Value()).Value();
        const auto metadata = ProjectPath::Parse(Identity().key.source.Value() + ".horo").Value();
        REQUIRE(registry.Publish({{Asset(), Assets::AssetTypeId::Parse("core.pcg_graph").Value(), source, metadata}}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
    }

    /** @brief Exercises the production workspace route with durable source and the real built-in catalog. */
    struct CanvasWorkspace final {
        Project project;
        Assets::AssetRegistry registry;
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        std::unique_ptr<JobSystem> jobs;
        std::unique_ptr<EditorWorkspaceController> controller;

        explicit CanvasWorkspace(bool preview = false, bool grantPreview = true) {
            PublishGraphAsset(registry);
            REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
            const std::array grants{PCG::PCGCapability::Validation, PCG::PCGCapability::EditorPreview, PCG::PCGCapability::OfflineBake,
                                    PCG::PCGCapability::RuntimeEvaluation, PCG::PCGCapability::HybridEvaluation};
            auto composed =
                preview && grantPreview ? CreatePCGEditorCatalog(PCG::PCGCapabilitySet::Create(grants).Value()) : CreatePCGEditorCatalog();
            REQUIRE(composed.HasValue());
            auto catalog = std::move(composed).Value();
            // The existing durable fixture contains one synthetic source type; preserve it alongside built-ins.
            catalog.document.types.push_back(Catalog().types.front());
            if (preview) {
                jobs = std::make_unique<JobSystem>(JobSystemConfig{.workerCount = 1});
                PCG::PCGGraphSourceData source;
                source.generation = {Id<PCG::GraphId>(1), Id<PCG::GraphRevision>(1)};
                auto node = catalog.palette.front().prototype;
                node.id = Id<PCG::NodeId>(1);
                source.nodes = {node};
                auto graph = PCG::PCGGraphAsset::Create(source, {.supportedNodeTypes = catalog.document.types});
                REQUIRE(graph.HasValue());
                auto bytes = PCG::SerializePCGGraphAsset(graph.Value()).Value();
                const auto raw = std::as_bytes(std::span(bytes));
                project.Write({{true, raw.size(), ComputeSha256(raw)}, {raw.begin(), raw.end()}});
            }
            controller = std::make_unique<
                EditorWorkspaceController>(project.root, runtime, registry.Snapshot(),
                                           EditorWorkspaceDependencies{.mutations = &project.mutations,
                                                                       .durableFiles = &project.files,
                                                                       .jobs = jobs.get(),
                                                                       .documentSessionOwner = Asset(9),
                                                                       .procedural =
                                                                           {.catalog = std::move(catalog),
                                                                            .previewInputs =
                                                                                preview ? std::optional{CreatePCGPreviewSandbox().Value()}
                                                                                        : std::nullopt}});

            Open();
        }

        void Open() {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::OpenSourceFile;
            command.sourceOpenRequest = SourceOpenRequest{.path = project.path, .mode = SourceOpenMode::EmbeddedOnly};
            controller->ProcessCommand(command);
            REQUIRE(controller->ViewModel().pcgDocuments.size() == 1);
        }

        const PCGWorkspaceDocument &View() const {
            return controller->ViewModel().pcgDocuments.front();
        }

        PCGEditorRequest Request(PCGEditorAction action = PCGEditorAction::Present) const {
            return {.action = action, .fence = Fence(View().snapshot), .presentation = View().canvas};
        }

        void Send(PCGEditorRequest request) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::EditPCGGraph;
            command.documentInstance = View().snapshot.identity.instance;
            command.pcgRequest = std::move(request);
            controller->ProcessCommand(command);
        }

        void Unchanged(const PCGDocumentSnapshot &before) const {
            const auto after = controller->PCGDocuments().Snapshot(before.identity.instance).Value();
            CHECK(after.source == before.source);
            CHECK(after.revision == before.revision);
            CHECK(after.state == before.state);
            CHECK(after.dirty == before.dirty);
        }
    };

}  // namespace Horo::Editor::PCGCanvasTestSupport
