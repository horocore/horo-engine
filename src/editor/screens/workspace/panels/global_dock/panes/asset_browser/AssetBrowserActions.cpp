#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserActions.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"

#include <algorithm>
#include <filesystem>
#include <ranges>

namespace Horo::Editor {
    namespace {
        void DrawPasteAction(const std::filesystem::path &destination, const ContentBrowserClipboardMode clipboardMode,
                             const EditorGuiContext &context, EditorWorkspaceViewCommandData &command) {
            const ILocalizationService &localization = context.localization;
            ImGui::BeginDisabled(clipboardMode == ContentBrowserClipboardMode::None);
            const bool activated = Ui::ContextMenuItem(localization
                                                           .Get("editor", clipboardMode == ContentBrowserClipboardMode::Move
                                                                              ? "workspace.content_browser.action.move_here"
                                                                              : "workspace.content_browser.action.paste_here")
                                                           .c_str(),
                                                       "Ctrl+V", context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.paste");
            ImGui::EndDisabled();
            if (activated)
                command = AssetBrowserInteractionSession::Paste(destination.string());
        }

        void DrawAssetClipboardActions(const ContentBrowserEntry &entry, const EditorGuiContext &context,
                                       EditorWorkspaceViewCommandData &command) {
            using enum Ui::ContextMenuItemTone;
            const ILocalizationService &localization = context.localization;
            if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.duplicate").c_str(), "Ctrl+D",
                                    context.theme.fonts, Normal, "action.duplicate"))
                command = AssetBrowserInteractionSession::Duplicate(entry.absolutePath);
            if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.copy").c_str(), "Ctrl+C",
                                    context.theme.fonts, Normal, "action.copy"))
                command = AssetBrowserInteractionSession::Copy(entry.absolutePath);
            if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.cut").c_str(), "Ctrl+X",
                                    context.theme.fonts, Normal, "action.cut"))
                command = AssetBrowserInteractionSession::Cut(entry.absolutePath);
        }

        void DrawDirectoryActions(const ContentBrowserEntry &entry, const EditorWorkspaceViewModel &viewModel,
                                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
            DrawPasteAction(entry.absolutePath, viewModel.contentBrowserClipboard.mode, context, command);
            if (const ILocalizationService &localization = context.localization;
                Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.import_here").c_str(), nullptr,
                                    context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.import"))
                command = AssetBrowserInteractionSession::ImportHere(entry.absolutePath);
            Ui::ContextMenuSeparator();
        }

        void DrawAssetEntrySpecificActions(const ContentBrowserEntry &entry, EditorWorkspaceViewCommandData &command,
                                           const EditorGuiContext &context) {
            if (entry.kind != ContentBrowserEntryKind::Asset)
                return;
            const ILocalizationService &localization = context.localization;
            if (std::filesystem::path{entry.absolutePath}.extension() == ".horo_script" &&
                Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.open_external_ide").c_str(), nullptr,
                                    context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.open")) {
                command.command = EditorWorkspaceViewCommand::OpenSourceFile;
                command.sourceOpenRequest = SourceOpenRequest{
                    .path = entry.absolutePath,
                    .origin = SourceOpenOrigin::AssetActivation,
                    .mode = SourceOpenMode::AllowExternalFallback,
                };
            }
            ImGui::BeginDisabled(!entry.canReimport);
            if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.reimport").c_str(), nullptr,
                                    context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.refresh"))
                command = AssetBrowserInteractionSession::Reimport(entry.absolutePath);
            ImGui::EndDisabled();
        }
    }  // namespace

    /** @copydoc DrawAssetBrowserBackgroundActions */
    void DrawAssetBrowserBackgroundActions(const EditorWorkspaceViewModel &viewModel, AssetBrowserInteractionSession &interactionSession,
                                           EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        if (!Ui::BeginContextWindowMenu("##ContentBrowserBackgroundMenu"))
            return;

        const ContentBrowserDirectory &directory = viewModel.contentBrowser;
        const ILocalizationService &localization = context.localization;
        DrawPasteAction(directory.absoluteCurrentPath, viewModel.contentBrowserClipboard.mode, context, command);
        Ui::ContextMenuSeparator();
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.create_folder") +
                                 "###content_browser_action_create_folder")
                                    .c_str(),
                                nullptr, context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.create")) {
            interactionSession.OpenCreateFolder();
        }
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.create_lua_behavior") +
                                 "###content_browser_create_lua_behavior")
                                    .c_str(),
                                nullptr, context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.create_lua_behavior")) {
            command.command = EditorWorkspaceViewCommand::CreateLuaBehavior;
            command.stringPayload = directory.absoluteCurrentPath;
        }
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.create_native_behavior") +
                                 "###content_browser_create_native_behavior")
                                    .c_str(),
                                nullptr, context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.create_native_behavior")) {
            command.command = EditorWorkspaceViewCommand::CreateNativeBehavior;
            command.stringPayload = directory.absoluteCurrentPath;
        }
        if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.import_here").c_str(), nullptr,
                                context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.import")) {
            command = AssetBrowserInteractionSession::ImportHere(directory.absoluteCurrentPath);
        }
        if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.refresh").c_str(), nullptr,
                                context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.refresh")) {
            command = AssetBrowserInteractionSession::Refresh();
        }
        Ui::EndContextMenu();
    }

    /** @copydoc DrawAssetBrowserEntryActions */
    void DrawAssetBrowserEntryActions(const ContentBrowserEntry &entry, const EditorWorkspaceViewModel &viewModel,
                                      AssetBrowserInteractionSession &interactionSession, EditorWorkspaceViewCommandData &command,
                                      const EditorGuiContext &context) {
        if (!Ui::BeginContextMenu("##ContentBrowserCardMenu"))
            return;

        if (entry.kind == ContentBrowserEntryKind::Asset) {
            DrawAssetClipboardActions(entry, context, command);
            Ui::ContextMenuSeparator();
        } else {
            DrawDirectoryActions(entry, viewModel, command, context);
        }
        const ILocalizationService &localization = context.localization;
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.rename") + "###content_browser_action_rename")
                                    .c_str(),
                                "F2", context.theme.fonts, Ui::ContextMenuItemTone::Normal, "action.rename")) {
            interactionSession.OpenRename(entry);
        }
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.asset_info") +
                                 "###content_browser_action_asset_info")
                                    .c_str(),
                                nullptr, context.theme.fonts)) {
            interactionSession.OpenInfo(entry);
        }
        DrawAssetEntrySpecificActions(entry, command, context);
        if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.reveal").c_str(), nullptr,
                                context.theme.fonts)) {
            command = AssetBrowserInteractionSession::Reveal(entry.absolutePath);
        }
        if (Ui::ContextMenuItem(localization.Get("editor", "workspace.content_browser.action.copy_path").c_str(), nullptr,
                                context.theme.fonts)) {
            ImGui::SetClipboardText(entry.absolutePath.c_str());
        }
        Ui::ContextMenuSeparator();
        if (Ui::ContextMenuItem((localization.Get("editor", "workspace.content_browser.action.delete") + "###content_browser_action_delete")
                                    .c_str(),
                                "Delete", context.theme.fonts, Ui::ContextMenuItemTone::Danger, "action.delete")) {
            interactionSession.OpenDelete(entry);
        }
        Ui::EndContextMenu();
    }

    /** @copydoc HandleAssetBrowserShortcuts */
    void HandleAssetBrowserShortcuts(const std::vector<std::size_t> &visibleEntries, const EditorWorkspaceViewModel &viewModel,
                                     AssetBrowserInteractionSession &interactionSession, EditorWorkspaceViewCommandData &command) {
        const ImGuiIO &io = ImGui::GetIO();
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || io.WantTextInput || ImGui::IsAnyItemActive())
            return;

        const AssetBrowserInteractionState &state = interactionSession.State();
        const ContentBrowserDirectory &directory = viewModel.contentBrowser;
        const auto selected = std::ranges::find(visibleEntries, state.selectedAbsolutePath, [&directory](const std::size_t entryIndex) {
            return directory.entries[entryIndex].absolutePath;
        });
        const bool selectedAsset = selected != visibleEntries.end() && directory.entries[*selected].kind == ContentBrowserEntryKind::Asset;
        const bool commandModifier = io.KeyCtrl || io.KeySuper;
        if (selected != visibleEntries.end() && ImGui::IsKeyPressed(ImGuiKey_F2))
            interactionSession.OpenRename(directory.entries[*selected]);
        else if (selected != visibleEntries.end() && ImGui::IsKeyPressed(ImGuiKey_Delete))
            interactionSession.OpenDelete(directory.entries[*selected]);
        else if (commandModifier && selectedAsset && ImGui::IsKeyPressed(ImGuiKey_D))
            command = AssetBrowserInteractionSession::Duplicate(state.selectedAbsolutePath);
        else if (commandModifier && selectedAsset && ImGui::IsKeyPressed(ImGuiKey_C))
            command = AssetBrowserInteractionSession::Copy(state.selectedAbsolutePath);
        else if (commandModifier && selectedAsset && ImGui::IsKeyPressed(ImGuiKey_X))
            command = AssetBrowserInteractionSession::Cut(state.selectedAbsolutePath);
        else if (commandModifier && ImGui::IsKeyPressed(ImGuiKey_V) &&
                 viewModel.contentBrowserClipboard.mode != ContentBrowserClipboardMode::None)
            command = AssetBrowserInteractionSession::Paste(directory.absoluteCurrentPath);
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && viewModel.contentBrowserClipboard.mode != ContentBrowserClipboardMode::None)
            command = AssetBrowserInteractionSession::CancelClipboard();
    }
}  // namespace Horo::Editor
