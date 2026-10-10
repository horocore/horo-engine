#include "EditorWorkspaceControllerPolicyTestSupport.h"
#include "SequenceDocumentTestSupport.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    using namespace Horo::Editor;

    struct SequenceProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-sequence-workspace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        SequenceProject() {
            std::filesystem::create_directories(root);
        }

        ~SequenceProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const char *name, const std::string &bytes) const {
            std::ofstream output(root / name, std::ios::binary);
            output << bytes;
            REQUIRE(static_cast<bool>(output));
        }
    };

    EditorWorkspaceViewCommandData Open(const char *name) {
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::OpenSourceFile;
        command.sourceOpenRequest = SourceOpenRequest{.path = name, .origin = SourceOpenOrigin::AssetActivation};
        return command;
    }

    void VerifyNativeFocusHandoff(HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController &controller,
                                  const DocumentInstanceId instance, const EditorWorkspaceViewCommandData &scrub) {
        EditorWorkspaceViewCommandData native;
        native.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        native.targetIndex = 3;
        native.stringPayload = "horo.viewport";
        controller.ProcessCommand(native);
        CHECK_FALSE(controller.ViewModel().workspacePanelHost.ActiveDocument());
        controller.ProcessCommand(scrub);
        CHECK(controller.ViewModel().sequenceDocuments.front().timeline.playhead == 100);
        EditorWorkspaceViewCommandData focus;
        focus.command = EditorWorkspaceViewCommand::FocusSequenceDocument;
        focus.documentInstance = instance;
        controller.ProcessCommand(focus);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
    }
}  // namespace

TEST_CASE("Sequence asset activation opens focuses and closes actual document tabs", "[unit][editor][sequence]") {
    SequenceProject project;
    project.Write("intro.hsequence", SequenceDocumentTests::Source());
    project.Write("alias.hsequence", SequenceDocumentTests::Source());
    bool external = false;
    HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController controller(project.root, {}, [&](const auto &) {
        external = true;
        return true;
    });
    controller.ProcessCommand(Open("intro.hsequence"));
    REQUIRE(controller.ViewModel().sequenceDocuments.size() == 1);
    REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
    const auto instance = controller.ViewModel().sequenceDocuments.front().source->identity.instance;
    CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);
    CHECK_FALSE(external);
    CHECK_FALSE(controller.ViewModel().isDirty);
    controller.ProcessCommand(Open("alias.hsequence"));
    REQUIRE(controller.ViewModel().sequenceDocuments.size() == 1);
    CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == instance);

    EditorWorkspaceViewCommandData scrub;
    scrub.command = EditorWorkspaceViewCommand::UpdateSequenceTimeline;
    scrub.documentInstance = instance;
    scrub.sequenceTimeline = SequenceTimelineState{.firstFrame = 60, .playhead = 100, .zoom = 2.0};
    controller.ProcessCommand(scrub);
    CHECK(controller.ViewModel().sequenceDocuments.front().timeline.playhead == 100);
    CHECK_FALSE(controller.ViewModel().isDirty);
    CHECK_FALSE(controller.ViewModel().canUndo);
    controller.ProcessCommand(Open("intro.hsequence"));
    CHECK(controller.ViewModel().sequenceDocuments.front().timeline.playhead == 100);
    VerifyNativeFocusHandoff(controller, instance, scrub);
    project.Write("intro.hsequence", SequenceDocumentTests::Source() + "\n");
    controller.ProcessCommand(Open("intro.hsequence"));
    CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.sequence.conflict");
    CHECK(controller.ViewModel().sequenceDocuments.size() == 1);
    EditorWorkspaceViewCommandData close;
    close.command = EditorWorkspaceViewCommand::CloseSequenceDocument;
    close.documentInstance = instance;
    controller.ProcessCommand(close);
    CHECK(controller.ViewModel().sequenceDocuments.empty());
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
    CHECK_FALSE(controller.ViewModel().workspacePanelHost.ActiveDocument());
    controller.ProcessCommand(Open("intro.hsequence"));
    REQUIRE(controller.ViewModel().sequenceDocuments.size() == 1);
    CHECK(controller.ViewModel().sequenceDocuments.front().source->identity.instance != instance);
    controller.ProcessCommand(scrub);
    CHECK(controller.ViewModel().sequenceDocuments.front().timeline.playhead == 0);
}

TEST_CASE("Malformed sequence activation rolls back its identity and leaves other tabs usable", "[unit][editor][sequence]") {
    SequenceProject project;
    project.Write("bad.hsequence", "{}");
    HoroEditorWorkspaceControllerPolicyTests::FocusedWorkspaceController controller(project.root);
    controller.ProcessCommand(Open("bad.hsequence"));
    CHECK(controller.ViewModel().sequenceDocuments.empty());
    CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
    CHECK(controller.ViewModel().contentBrowserOperationError == "workspace.sequence.invalid");
    project.Write("bad.hsequence", SequenceDocumentTests::Source());
    controller.ProcessCommand(Open("bad.hsequence"));
    REQUIRE(controller.ViewModel().sequenceDocuments.size() == 1);
}
