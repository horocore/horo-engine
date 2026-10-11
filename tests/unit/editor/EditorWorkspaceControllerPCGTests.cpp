#include "PCGDocumentTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;

    TEST_CASE("Workspace PCG activation owns binary sessions dirty tabs recovery save and close", "[unit][editor][pcg][workspace]") {
        Project project;
        Assets::AssetRegistry registry;
        const auto source = ProjectPath::Parse(Identity().key.source.Value()).Value();
        const auto metadata = ProjectPath::Parse(Identity().key.source.Value() + ".horo").Value();
        REQUIRE(registry.Publish({{Asset(), Assets::AssetTypeId::Parse("core.pcg_graph").Value(), source, metadata}}).status ==
                Assets::AssetRegistryBuildStatus::Complete);
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
        EditorWorkspaceController controller(project.root, runtime, registry.Snapshot(),
                                             {.mutations = &project.mutations,
                                              .durableFiles = &project.files,
                                              .documentSessionOwner = Asset(9),
                                              .pcgCatalog = Catalog()});
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = project.path, .mode = SourceOpenMode::EmbeddedOnly};
        controller.ProcessCommand(open);
        auto documents = controller.PCGDocuments().Snapshots();
        REQUIRE(documents.HasValue());
        REQUIRE(documents.Value().size() == 1);
        const auto initial = documents.Value().front();
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == initial.identity.instance);
        REQUIRE(controller.ExecutePCGDocument(initial.identity.instance, Edit(initial, 42)).HasValue());
        CHECK(controller.HasDirtyPCGDocuments());
        CHECK_FALSE(controller.ViewModel().isDirty);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        controller.ProcessCommand(open);
        CHECK(controller.PCGDocuments().Snapshots().Value().size() == 1);
        controller.UpdateAutosave(61, 1);
        REQUIRE(controller.PCGDocuments().InspectRecovery(initial.identity.instance).Value());
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        ErrorIs(controller.ClosePCGDocument(initial.identity.instance, false), PCGDocumentError(PCGDocumentCommandFailureReason::Dirty));
        project.files.failWrite = true;
        REQUIRE(controller.SavePCGDocuments().HasError());
        CHECK(controller.HasDirtyPCGDocuments());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        project.files.failWrite = false;
        REQUIRE(controller.SavePCGDocuments().HasValue());
        CHECK_FALSE(controller.HasDirtyPCGDocuments());
        CHECK_FALSE(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        REQUIRE(controller.ClosePCGDocument(initial.identity.instance, false).HasValue());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
        controller.ProcessCommand(open);
        auto reopened = controller.PCGDocuments().Snapshots().Value().front();
        CHECK(reopened.session != initial.session);
        CHECK(reopened.source->Data().deterministicSeed == 42);
        ErrorIs(controller.ExecutePCGDocument(reopened.identity.instance, Edit(initial, 43)),
                PCGDocumentError(PCGDocumentCommandFailureReason::Stale));
        REQUIRE(controller.ShutdownPCGDocuments().HasValue());
    }
}  // namespace Horo::Editor
