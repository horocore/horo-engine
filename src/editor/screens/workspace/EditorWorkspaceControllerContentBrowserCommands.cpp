#include "Horo/Foundation/Logging/Logger.h"
#include "WorkspacePathText.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

namespace Horo::Editor {

    bool EditorWorkspaceController::ProcessContentBrowserAssetCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::RenameContentBrowserEntry:
                if (cmd.stringPayload.has_value() && cmd.secondaryStringPayload.has_value())
                    RenameContentBrowserEntry(WorkspaceNativePath(*cmd.stringPayload), *cmd.secondaryStringPayload);
                break;
            case EditorWorkspaceViewCommand::DeleteContentBrowserEntry:
                if (cmd.stringPayload.has_value())
                    DeleteContentBrowserEntry(WorkspaceNativePath(*cmd.stringPayload));
                break;
            case EditorWorkspaceViewCommand::DuplicateContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    DuplicateContentBrowserAsset(WorkspaceNativePath(*cmd.stringPayload));
                break;
            case EditorWorkspaceViewCommand::CopyContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    SetContentBrowserClipboard(WorkspaceNativePath(*cmd.stringPayload), ContentBrowserClipboardMode::Copy);
                break;
            case EditorWorkspaceViewCommand::CutContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    SetContentBrowserClipboard(WorkspaceNativePath(*cmd.stringPayload), ContentBrowserClipboardMode::Move);
                break;
            case EditorWorkspaceViewCommand::PasteContentBrowserAsset:
                PasteContentBrowserAsset(WorkspaceNativePath(cmd.stringPayload.value_or(m_viewModel.contentBrowser.absoluteCurrentPath)));
                break;
            case EditorWorkspaceViewCommand::TransferContentBrowserAsset:
                if (cmd.contentBrowserTransfer.has_value())
                    TransferContentBrowserAsset(*cmd.contentBrowserTransfer);
                break;
            case EditorWorkspaceViewCommand::CancelContentBrowserClipboard:
                ClearContentBrowserClipboard();
                break;
            case EditorWorkspaceViewCommand::CreateContentBrowserFolder:
                if (cmd.stringPayload.has_value() && cmd.secondaryStringPayload.has_value())
                    CreateContentBrowserFolder(WorkspaceNativePath(*cmd.stringPayload), *cmd.secondaryStringPayload);
                break;
            default:
                return false;
        }
        return true;
    }

    bool EditorWorkspaceController::ProcessContentBrowserSourceCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::CreateLuaBehavior:
                if (cmd.gameplayBehaviorRequest.has_value()) {
                    if (cmd.gameplayBehaviorRequest->kind != GameplayBehaviorKind::Lua) {
                        m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
                        break;
                    }
                    CreateGameplayBehavior(*cmd.gameplayBehaviorRequest);
                } else {
                    LOG_WARN("editor.asset_actions",
                             "CreateLuaBehavior command received without explicit request details; modal prompt expected.");
                }
                break;
            case EditorWorkspaceViewCommand::CreateNativeBehavior:
                if (cmd.gameplayBehaviorRequest.has_value()) {
                    if (cmd.gameplayBehaviorRequest->kind != GameplayBehaviorKind::Native) {
                        m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
                        break;
                    }
                    CreateGameplayBehavior(*cmd.gameplayBehaviorRequest);
                } else {
                    LOG_WARN("editor.asset_actions",
                             "CreateNativeBehavior command received without explicit request details; modal prompt expected.");
                }
                break;
            case EditorWorkspaceViewCommand::ReimportContentBrowserAsset:
                if (cmd.stringPayload.has_value())
                    ReimportContentBrowserAsset(WorkspaceNativePath(*cmd.stringPayload));
                break;
            case EditorWorkspaceViewCommand::RevealContentBrowserEntry:
                if (cmd.stringPayload.has_value())
                    RevealContentBrowserEntry(WorkspaceNativePath(*cmd.stringPayload));
                break;
            case EditorWorkspaceViewCommand::OpenSourceFile:
                if (cmd.sourceOpenRequest.has_value())
                    OpenSourceFile(*cmd.sourceOpenRequest);
                break;
            case EditorWorkspaceViewCommand::OpenDiagnosticSource:
                if (cmd.diagnosticSource.has_value())
                    OpenDiagnosticSource(*cmd.diagnosticSource);
                break;
            default:
                return false;
        }
        return true;
    }

}  // namespace Horo::Editor
