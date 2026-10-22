#include "Horo/Editor/GuiScreen.h"
#include "PrefabWorkspaceTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceView.h"
#include "editor_ui/HeadlessEditorGuiFixture.h"

#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Editor {
    using namespace PrefabWorkspaceTestSupport;

    namespace {
        /** @brief Detects any attempted Scene command escape from the read-only authoring viewport. */
        class PanelProbe final : public IWorkspacePanel {
        public:
            explicit PanelProbe(std::string id, WorkspaceDockArea area) : id_(std::move(id)), area_(area) {}

            std::size_t draws{};

            std::string GetId() const override {
                return id_;
            }

            std::string GetDisplayName() const override {
                return id_;
            }

            WorkspaceDockArea GetDefaultDockArea() const override {
                return area_;
            }

            std::vector<std::string> GetObservedEventTypes() const override {
                return {};
            }

            void OnAttach(PanelContext &) override {}

            void OnDetach() override {}

            void DrawIcon(ImDrawList *, const ImVec2 &, const ImVec2 &, ImU32) override {}

            void DrawPanel(const ImVec2 &, const ImVec2 &, const EditorWorkspaceViewModel &, EditorWorkspaceViewCommandData &command,
                           const EditorGuiContext &) override {
                ++draws;
                command.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
            }

        private:
            std::string id_;
            WorkspaceDockArea area_;
        };

    }  // namespace

    TEST_CASE("Prefab source edits and focus retirement preserve the Scene authority", "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto scene = owner.ViewModel().documentRevision;
        owner.ProcessCommand(
            Intent(owner.ViewModel(), owner.ViewModel().prefabHierarchy->entries.front().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        const auto initial = owner.ViewModel().prefabDocuments.front();
        EditorWorkspaceViewCommandData edit;
        edit.command = EditorWorkspaceViewCommand::PrefabDocument;
        edit.prefabFence = initial.fence;
        edit.prefabEdit = RenamePrefabObject{{0}, "Source edit"};
        owner.ProcessCommand(edit);
        REQUIRE(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().prefabDocuments.front().stageActive);
        CHECK(owner.ViewModel().documentRevision == scene);
        CHECK_FALSE(owner.ViewModel().isDirty);
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = initial.identity.instance});
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        CHECK(owner.ViewModel().prefabDocuments.front().dirty);
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK_FALSE(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().documentRevision == scene);
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = initial.identity.instance});
        CHECK(owner.ViewModel().prefabDocuments.empty());
        CHECK(initial.source->Data().objects.front().name == "Source root");
    }

    TEST_CASE("Typed prefab asset activation focuses its persistent document and preserves dirty history",
              "[editor][prefab][assets][native]") {
        Workspace workspace("prefab asset ü");
        auto &owner = *workspace.controller;
        const auto sceneRevision = owner.ViewModel().documentRevision;
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = workspace.project.source,
                                                   .origin = SourceOpenOrigin::AssetActivation,
                                                   .mode = SourceOpenMode::EmbeddedOnly};
        owner.ProcessCommand(open);
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        const auto initial = owner.ViewModel().prefabDocuments.front();
        CHECK(initial.identity.key.kind == DocumentKind::Prefab);
        CHECK_FALSE(workspace.navigated.has_value());
        EditorWorkspaceViewCommandData edit;
        edit.command = EditorWorkspaceViewCommand::PrefabDocument;
        edit.prefabFence = initial.fence;
        edit.prefabEdit = RenamePrefabObject{{0}, "Browser source edit"};
        owner.ProcessCommand(edit);
        REQUIRE(owner.ViewModel().prefabDocuments.front().dirty);
        owner.ProcessCommand(open);
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        CHECK(owner.ViewModel().prefabDocuments.front().identity.instance == initial.identity.instance);
        CHECK(owner.ViewModel().prefabDocuments.front().dirty);
        EditorWorkspaceViewCommandData save;
        save.command = EditorWorkspaceViewCommand::PrefabDocument;
        save.prefabFence = owner.ViewModel().prefabDocuments.front().fence;
        save.prefabAction = PrefabDocumentAction::Save;
        owner.ProcessCommand(save);
        CHECK_FALSE(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().prefabDocuments.front().source->Data().objects.front().name == "Browser source edit");
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument, .documentInstance = initial.identity.instance});
        REQUIRE(owner.ViewModel().prefabDocuments.empty());
        CHECK_FALSE(owner.ViewModel().workspacePanelHost.ActiveDocument().has_value());
        owner.ProcessCommand(open);
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        CHECK(owner.ViewModel().prefabDocuments.front().identity.instance != initial.identity.instance);
        CHECK_FALSE(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().prefabDocuments.front().source->Data().objects.front().name == "Browser source edit");
        CHECK(owner.ViewModel().documentRevision == sceneRevision);
        CHECK_FALSE(owner.ViewModel().isDirty);
    }

    TEST_CASE("Switching prefab documents retires preview without losing source history or granting Scene edits",
              "[editor][prefab][native]") {
        Workspace workspace;
        auto &owner = *workspace.controller;
        const auto scene = owner.ViewModel().documentRevision;
        owner.ProcessCommand(
            Intent(owner.ViewModel(), owner.ViewModel().prefabHierarchy->entries.front().target, PrefabHierarchyAction::OpenSource));
        REQUIRE(owner.ViewModel().prefabDocuments.size() == 1);
        const auto initial = owner.ViewModel().prefabDocuments.front();
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::PrefabDocument,
                              .prefabFence = initial.fence,
                              .prefabEdit = RenamePrefabObject{{0}, "Retained source"}});
        REQUIRE(owner.ViewModel().prefabDocuments.front().dirty);
        owner.ProcessCommand(
            Intent(owner.ViewModel(), owner.ViewModel().prefabHierarchy->entries.front().target, PrefabHierarchyAction::DeleteInstance));
        CHECK(owner.ViewModel().documentRevision == scene);
        REQUIRE(owner.ViewModel().prefabHierarchy->entries.size() == 3);
        const auto preview = owner.ViewModel().prefabDocuments.front().previewRevision;
        const auto code = workspace.project.directory.path / "note.cpp";
        std::ofstream(code) << "int value;\n";
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::OpenSourceFile, .sourceOpenRequest = SourceOpenRequest{.path = code}});
        CHECK_FALSE(owner.ViewModel().prefabDocuments.front().stageActive);
        CHECK(owner.ViewModel().prefabDocuments.front().dirty);
        owner.ProcessCommand(
            {.command = EditorWorkspaceViewCommand::FocusWorkspaceDocument, .documentInstance = initial.identity.instance});
        CHECK(owner.ViewModel().prefabDocuments.front().stageActive);
        CHECK(owner.ViewModel().prefabDocuments.front().previewRevision > preview);
        CHECK(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().prefabDocuments.front().canUndo);
        CHECK(owner.ViewModel().documentRevision == scene);
    }

    TEST_CASE("Prefab workspace routes save undo redo and explicit reload to source authority", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.OpenPrefab();
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        const auto scene = model.documentRevision;
        workspace.Rename("Saved source");
        REQUIRE(owner.HasDirtyPrefabDocuments());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::UndoScene});
        CHECK_FALSE(owner.HasDirtyPrefabDocuments());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::RedoScene});
        REQUIRE(owner.HasDirtyPrefabDocuments());
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::SaveScene});
        REQUIRE_FALSE(model.prefabDocumentError);
        CHECK_FALSE(owner.HasDirtyPrefabDocuments());
        workspace.Action(PrefabDocumentAction::Reload);
        REQUIRE_FALSE(model.prefabDocumentError);
        CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "Saved source");
        workspace.Rename("Discarded intent");
        workspace.Action(PrefabDocumentAction::Reload);
        CHECK(model.prefabDocumentError.has_value());
        workspace.Action(PrefabDocumentAction::DiscardAndReload);
        REQUIRE_FALSE(model.prefabDocumentError);
        CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "Saved source");
        workspace.Rename("Batch save");
        REQUIRE(owner.SavePrefabDocuments().HasValue());
        REQUIRE(owner.SavePrefabDocuments().HasValue());
        CHECK_FALSE(owner.HasDirtyPrefabDocuments());
        CHECK(model.documentRevision == scene);
        CHECK_FALSE(model.isDirty);
        REQUIRE(owner.ShutdownPrefabDocuments().HasValue());
        CHECK(model.prefabDocuments.empty());
    }

    TEST_CASE("Prefab workspace recovery survives shutdown and exact candidate decisions retain local intent", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.OpenPrefab();
        workspace.Rename("Durable recovery");
        auto &first = *workspace.controller;
        first.UpdatePrefabAutosave(-1, 1);
        first.UpdatePrefabAutosave(1, 1);
        CHECK(first.ViewModel().prefabDocuments.front().recoveryRevision == 0);
        first.UpdatePrefabAutosave(59, 1);
        REQUIRE(first.ViewModel().prefabRecovery);
        CHECK(first.ViewModel().prefabDocuments.front().recoveryRevision != 0);
        REQUIRE(first.ShutdownPrefabDocuments().HasValue());
        workspace.Open(PrefabAsset(10).Asset());
        workspace.OpenPrefab();
        const auto candidate = workspace.controller->ViewModel().prefabRecovery;
        REQUIRE(candidate);
        workspace.Action(PrefabDocumentAction::Recover, candidate);
        REQUIRE_FALSE(workspace.controller->ViewModel().prefabDocumentError);
        CHECK(workspace.controller->ViewModel().prefabDocuments.front().dirty);
        CHECK(workspace.controller->ViewModel().prefabDocuments.front().source->Data().objects.front().name == "Durable recovery");
        workspace.controller->UpdatePrefabAutosave(0, 0);
        REQUIRE(workspace.controller->ViewModel().prefabRecovery);
        const auto replacement = workspace.controller->ViewModel().prefabRecovery;
        workspace.Action(PrefabDocumentAction::DiscardRecovery, candidate);
        CHECK(workspace.controller->ViewModel().prefabDocumentError.has_value());
        workspace.Action(PrefabDocumentAction::DiscardRecovery, replacement);
        REQUIRE_FALSE(workspace.controller->ViewModel().prefabDocumentError);
        CHECK_FALSE(workspace.controller->ViewModel().prefabRecoveryAvailable);
        CHECK(workspace.controller->ViewModel().prefabDocuments.front().dirty);
        workspace.Action(PrefabDocumentAction::DiscardAndClose);
        CHECK(workspace.controller->ViewModel().prefabDocuments.empty());
    }

    TEST_CASE("Prefab workspace rejects stale commands and preserves source under external refresh", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.OpenPrefab();
        auto &owner = *workspace.controller;
        const auto &model = owner.ViewModel();
        const auto original = model.prefabDocuments.front();
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::PrefabDocument});
        workspace.Rename("Local source");
        owner.ProcessCommand({.command = EditorWorkspaceViewCommand::PrefabDocument,
                              .prefabFence = original.fence,
                              .prefabAction = PrefabDocumentAction::Undo});
        CHECK(model.prefabDocumentError.has_value());
        CHECK(model.prefabDocuments.front().dirty);
        workspace.project.WriteSource("External source");
        owner.RefreshAssets(workspace.project.registry.Snapshot());
        const bool unchangedPublicationIgnored = !model.prefabDocuments.front().externalConflict;
        const auto publication = workspace.project.registry.Publish({workspace.project.record});
        owner.RefreshAssets(workspace.project.registry.Snapshot());
        CHECK((unchangedPublicationIgnored && publication.status == Assets::AssetRegistryBuildStatus::Complete &&
               model.prefabDocuments.front().externalConflict));
        workspace.Action(PrefabDocumentAction::Save);
        CHECK(model.prefabDocumentError.has_value());
        workspace.Action(PrefabDocumentAction::DiscardAndReload);
        REQUIRE_FALSE(model.prefabDocumentError);
        CHECK(model.prefabDocuments.front().source->Data().objects.front().name == "External source");
        workspace.Action(PrefabDocumentAction::Recover);
        CHECK(model.prefabDocumentError.has_value());
        workspace.Rename("Explicit discard");
        owner.UpdatePrefabAutosave(0, 0);
        REQUIRE(owner.ShutdownPrefabDocuments(true).HasValue());
        CHECK(model.prefabDocuments.empty());
        CHECK(original.source->Data().objects.front().name == "Source root");
    }

    TEST_CASE("Missing prefab persistence capabilities fail closed without losing editable source", "[editor][prefab][native]") {
        Workspace workspace;
        workspace.persistenceAvailable = false;
        workspace.Open(PrefabAsset(10).Asset());
        workspace.OpenPrefab();
        workspace.Rename("Retained without writer");
        auto &owner = *workspace.controller;
        workspace.Action(PrefabDocumentAction::Save);
        REQUIRE(owner.ViewModel().prefabDocumentError);
        CHECK(owner.ViewModel().prefabDocumentError->code.Value() == Assets::AssetSourceErrors::Authority.code.Value());
        REQUIRE(owner.SavePrefabDocuments().HasError());
        REQUIRE(owner.ShutdownPrefabDocuments().HasError());
        owner.UpdatePrefabAutosave(0, 0);
        workspace.Action(PrefabDocumentAction::Recover);
        REQUIRE(owner.ViewModel().prefabDocumentError);
        CHECK(owner.ViewModel().prefabDocumentError->code.Value() == Assets::AssetSourceErrors::Authority.code.Value());
        CHECK(owner.ViewModel().prefabDocuments.front().dirty);
        CHECK(owner.ViewModel().prefabDocuments.front().source->Data().objects.front().name == "Retained without writer");
        REQUIRE(owner.ShutdownPrefabDocuments(true).HasValue());
    }

    TEST_CASE("Prefab workspace view renders immutable source controls and fences read-only viewport commands",
              "[editor][prefab][native][gui]") {
        Workspace workspace;
        workspace.OpenPrefab();
        Tests::EditorGuiContextFixture gui{{1280, 1100}};
        ImGui::GetIO().IniFilename = nullptr;
        gui.settings.settings = DefaultEditorSettings();
        Theme::ApplyCurrentTheme();
        WorkspacePanelRegistry panels;
        auto viewport = std::make_shared<PanelProbe>("horo.viewport", WorkspaceDockArea::Document);
        auto hierarchy = std::make_shared<PanelProbe>("horo.hierarchy", WorkspaceDockArea::Left);
        auto inspector = std::make_shared<PanelProbe>("horo.inspector", WorkspaceDockArea::Right);
        panels.RegisterPanel(viewport);
        panels.RegisterPanel(hierarchy);
        panels.RegisterPanel(inspector);
        auto input = gui.input.PushContext(Input::InputContextId{"prefab-view"}, Input::InputContextKind::EditorWorkspace);
        EditorWorkspaceView view{gui.context, panels, 0, gui.input, input};
        auto model = workspace.controller->ViewModel();
        const auto original = model.prefabDocuments.front();
        for (int frame = 0; frame < 4; ++frame) {
            if (frame == 2) {
                model.prefabDocuments.front().stageActive = false;
                model.prefabDocuments.front().externalConflict = true;
                model.prefabDocumentError = MakeError(Assets::AssetSourceErrors::Conflict);
            }
            gui.imgui.BeginFrame();
            EditorWorkspaceViewCommandData command;
            view.Draw(model, command, {.width = 1280, .height = 1100});
            gui.imgui.EndFrame();
            CHECK(command.command == EditorWorkspaceViewCommand::None);
            REQUIRE(ImGui::GetDrawData());
            CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
        }
        CHECK(viewport->draws >= 1);
        CHECK(hierarchy->draws == 0);
        CHECK(inspector->draws == 0);
        CHECK(model.prefabDocuments.front().source == original.source);
        CHECK(model.prefabDocuments.front().fence.revision == original.fence.revision);
        CHECK_FALSE(workspace.controller->HasDirtyPrefabDocuments());
    }

}  // namespace Horo::Editor
