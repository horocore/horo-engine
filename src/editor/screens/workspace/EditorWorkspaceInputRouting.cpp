#include "EditorWorkspaceInputRouting.h"

#include "editor/input/EditorInputActions.h"

#include <array>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Runs scene shortcuts only after the workspace's application/text/popup priority has admitted them. */
        void RouteSceneInput(Input::InputRouter &router, const Input::InputContextToken &context, const EditorWorkspaceViewModel &model,
                             EditorWorkspaceViewCommandData &command) {
            const auto pressed = [&](const char *id) {
                return router.ReadAction(context, Input::ActionId{id}).pressed;
            };
            if (pressed(kActionRedo)) {
                command.command = EditorWorkspaceViewCommand::RedoScene;
                return;
            }
            if (pressed(kActionUndo)) {
                command.command = EditorWorkspaceViewCommand::UndoScene;
                return;
            }
            constexpr std::array tools{std::pair{kActionToolSelect, EditorTransformTool::Select},
                                       std::pair{kActionToolMove, EditorTransformTool::Move},
                                       std::pair{kActionToolRotate, EditorTransformTool::Rotate},
                                       std::pair{kActionToolScale, EditorTransformTool::Scale}};
            for (const auto &[action, tool] : tools) {
                if (pressed(action)) {
                    command.command = EditorWorkspaceViewCommand::ChangeTransformTool;
                    command.transformToolPayload = tool;
                    return;
                }
            }
            if (!model.primarySelection)
                return;
            if (pressed(kActionDuplicate)) {
                command.command = EditorWorkspaceViewCommand::DuplicateObject;
                command.objectPayload = model.primarySelection;
            } else if (pressed(kActionDelete)) {
                command.command = EditorWorkspaceViewCommand::DeleteObject;
                command.objectPayload = model.primarySelection;
            }
        }
    }  // namespace

    /** @copydoc RouteWorkspaceInputAction */
    void RouteWorkspaceInputAction(Input::InputRouter &router, const Input::InputContextToken &context,
                                   const EditorWorkspaceViewModel &model, EditorWorkspaceViewCommandData &command,
                                   const WorkspaceInputCapture capture) {
        const auto pressed = [&](const char *id) {
            return router.ReadAction(context, Input::ActionId{id}).pressed;
        };
        if (pressed(kActionSave)) {
            command.command = EditorWorkspaceViewCommand::SaveScene;
            for (const auto &source : model.sourceDocuments) {
                if (source.Identity().instance == model.workspacePanelHost.ActiveDocument()) {
                    command.command = EditorWorkspaceViewCommand::SaveSourceDocument;
                    command.sourceSave = SourceSaveRequest{source.Identity().instance, source.Revision(), {}};
                    break;
                }
            }
            return;
        }
        // Text owners route their own edit shortcuts; application save remains available above.
        if (command.sourceTextInputOwned || capture.textInputRequested || capture.popupOpen)
            return;
        RouteSceneInput(router, context, model, command);
    }
}  // namespace Horo::Editor
