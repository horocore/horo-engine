#include "EditorWorkspaceScreen.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Foundation/Paths.h"
#include "editor/modals/gameplay_behavior/GameplayBehaviorFilenameModal.h"
#include "editor/modals/packages/PackageModal.h"
#include "editor/modals/play_topology/PlayTopologyModal.h"
#include "editor/modals/prefab_creation/PrefabCreationModal.h"
#include "editor/modals/scene_compare/SceneConflictCompareModal.h"
#include "editor/modals/source_document/SourceDocumentCloseModal.h"
#include "editor/modals/world_layer/CreateWorldLayerModal.h"

#include <algorithm>
#include <format>
#include <portable-file-dialogs.h>
#include <span>
#include <utility>

namespace Horo::Editor::WorkspaceScreenDetail {
    namespace {
        /** @brief Formats bounded creation diagnostics after the workspace lifetime fence passes. */
        std::string PrefabCreationFailureMessage(const PrefabCreationOutcome *outcome, const ILocalizationService &localization) {
            std::string message = localization.Get("editor", "workspace.prefab.create.failed");
            if (!outcome)
                return message;
            if (outcome->error)
                message += std::format("\n{}", outcome->error->message);
            constexpr std::size_t maximumVisibleBoundaries{32};
            const auto count = std::min(outcome->boundaries.size(), maximumVisibleBoundaries);
            for (const auto &boundary : std::span{outcome->boundaries}.first(count)) {
                message += std::format("\n{}: {}", boundary.source.value, boundary.property);
                if (boundary.target)
                    message += std::format(" -> {}", boundary.target->value);
            }
            if (count < outcome->boundaries.size())
                message += std::format("\n{} {}", outcome->boundaries.size() - count,
                                       localization.Get("editor", "workspace.prefab.create.more_diagnostics"));
            for (const auto &receipt : outcome->files)
                if (receipt.recoveryError)
                    message += std::format("\n{}: {}", receipt.path.generic_string(), receipt.recoveryError->message);
            return message;
        }

        /** @brief Resolves an existing path where possible while retaining lexical fallback on canonicalization errors. */
        [[nodiscard]] std::filesystem::path NormalizeAbsolutePath(const std::filesystem::path &path) {
            std::error_code error;
            const std::filesystem::path absolute = std::filesystem::absolute(path, error).lexically_normal();
            if (error) {
                return {};
            }
            const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
            return error ? absolute : canonical;
        }

        /** @brief Uses the shared component-wise path containment policy. */
        [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate) {
            return Horo::Foundation::Paths::HasPathPrefix(root, candidate);
        }

        /** @brief Admits a behavior destination within the owning project or script root. */
        [[nodiscard]] std::optional<std::filesystem::path> ResolveGameplayBehaviorDestination(const std::filesystem::path &projectRoot,
                                                                                              const GameplayBehaviorKind kind,
                                                                                              const std::string &requestedDirectory) {
            using enum GameplayBehaviorKind;
            const std::filesystem::path normalizedProjectRoot = NormalizeAbsolutePath(projectRoot);
            const std::filesystem::path assetsRoot = ProjectLayout::AssetRoot(normalizedProjectRoot);
            const std::filesystem::path scriptsRoot = ProjectLayout::ScriptsRoot(normalizedProjectRoot);
            const std::filesystem::path requested = NormalizeAbsolutePath(requestedDirectory);
            std::filesystem::path destination;
            if (kind == Native) {
                destination = normalizedProjectRoot / "source" / "gameplay";
            } else if (HasPathPrefix(scriptsRoot, requested)) {
                destination = requested;
            } else {
                destination = scriptsRoot;
            }

            if ((kind == Native && !HasPathPrefix(normalizedProjectRoot, destination)) ||
                (kind == Lua && !HasPathPrefix(assetsRoot, destination))) {
                return std::nullopt;
            }
            return destination;
        }

        /** @brief Finds an unused generated behavior name with bounded filesystem probing. */
        [[nodiscard]] std::string SuggestGameplayBehaviorBaseName(const std::filesystem::path &destination,
                                                                  const GameplayBehaviorKind kind) {
            using enum GameplayBehaviorKind;
            const std::string extension = kind == Native ? ".cpp" : ".horo_script";
            std::error_code error;
            for (std::size_t suffix = 0; suffix < 1000; ++suffix) {
                if (const std::string stem = suffix == 0 ? "NewBehavior" : std::format("NewBehavior{}", suffix + 1);
                    !std::filesystem::exists(destination / (stem + extension), error)) {
                    return stem;
                }
                if (error) {
                    break;
                }
            }
            return "NewBehavior";
        }
    }  // namespace

    /** @copydoc WorkspaceSourceDocumentCommands::HandleMenuAction */
    bool WorkspaceSourceDocumentCommands::HandleMenuAction(EditorWorkspaceController *controller, const EditorMenuAction action) const {
        if (controller && (action == EditorMenuAction::SaveScene || action == EditorMenuAction::Undo || action == EditorMenuAction::Redo)) {
            const auto &model = controller->ViewModel();
            for (const auto &source : model.sourceDocuments) {
                if (source.Identity().instance != model.workspacePanelHost.ActiveDocument())
                    continue;
                EditorWorkspaceViewCommandData command;
                if (action == EditorMenuAction::SaveScene) {
                    command.command = EditorWorkspaceViewCommand::SaveSourceDocument;
                    command.sourceSave = SourceSaveRequest{source.Identity().instance, source.Revision(), {}};
                } else {
                    command.command = EditorWorkspaceViewCommand::SourceDocumentInteraction;
                    command.sourceInteraction = SourceHistoryRequest{source.Identity().instance, source.Revision(),
                                                                     action == EditorMenuAction::Undo ? SourceHistoryDirection::Undo
                                                                                                      : SourceHistoryDirection::Redo};
                }
                controller->ProcessCommand(command);
                return true;
            }
        }
        return false;
    }

    /** @copydoc EditorWorkspaceScreen::HandleMenuInvocation */
    bool EditorWorkspaceScreen::HandleMenuInvocation(const EditorMenuInvocation &invocation) {
        EditorMenuAction action = invocation.action;
        if (action == EditorMenuAction::ManagePackages) {
            OpenPackagesModal();
            return true;
        }
        if (sourceCommands_.HandleMenuAction(controller_.get(), action))
            return true;
        if (controller_ && action == EditorMenuAction::SaveScene && !controller_->HasActivePCGDocument() &&
            !controller_->CurrentScenePath().has_value())
            action = EditorMenuAction::SaveSceneAs;
        if (controller_ && action == EditorMenuAction::CreatePrimitive && invocation.primitive.has_value()) {
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::CreatePrimitive;
            command.primitivePayload = invocation.primitive;
            command.objectPayload = controller_->ViewModel().primarySelection;
            controller_->ProcessCommand(command);
            PublishViewportSceneIfChanged();
            return true;
        }
        if (controller_ && (action == EditorMenuAction::SaveSceneAs || action == EditorMenuAction::SaveSceneCopyAs)) {
            return SaveSceneAs(action == EditorMenuAction::SaveSceneCopyAs);
        }
        if (!controller_ ||
            (action != EditorMenuAction::SaveScene && action != EditorMenuAction::Undo && action != EditorMenuAction::Redo)) {
            return false;
        }

        using enum EditorMenuAction;
        EditorWorkspaceViewCommandData command;
        if (action == SaveScene) {
            command.command = EditorWorkspaceViewCommand::SaveScene;
        } else if (action == Undo) {
            command.command = EditorWorkspaceViewCommand::UndoScene;
        } else {
            command.command = EditorWorkspaceViewCommand::RedoScene;
        }
        controller_->ProcessCommand(command);
        PublishViewportSceneIfChanged();
        return true;
    }

    /** @copydoc EditorWorkspaceScreen::SaveSceneAs */
    bool EditorWorkspaceScreen::SaveSceneAs(const bool copyOnly) {
        const auto &currentPath = controller_->CurrentScenePath();
        const std::filesystem::path projectRoot{controller_->ViewModel().projectRoot};
        const std::filesystem::path suggestedPath = currentPath.value_or(ProjectLayout::ScenesRoot(projectRoot) / "main.horo");

        auto nativeDialogContext =
            input_.router.PushContext(Input::InputContextId{"editor.native_dialog.scene_save"}, Input::InputContextKind::NativeDialog);
#if defined(__APPLE__)
        // portable-file-dialogs forwards this value to AppleScript's
        // `default name`, where a full path is interpreted as a literal
        // filename and its separators become colons.
        const std::string dialogDefault = suggestedPath.filename().string();
#else
        const std::string dialogDefault = suggestedPath.string();
#endif
        pfd::save_file dialog(std::string{context_.localization.Get("editor", copyOnly ? "workspace.scene_save_copy_as.title"
                                                                                       : "workspace.scene_save_as.title")},
                              dialogDefault,
                              {std::string{context_.localization.Get("editor", "workspace.scene_save.file_type")}, "*.horo"});
        const std::string selected = dialog.result();
        if (selected.empty()) {
            return true;
        }

        std::filesystem::path destination{selected};
        if (destination.extension().empty()) {
            destination += ".horo";
        }
        if (!destination.is_absolute()) {
            LOG_ERROR("editor.scene_document", "Native scene destination dialog returned a non-absolute path '%s'.",
                      destination.string().c_str());
            return true;
        }
        std::error_code canonicalError;
        const std::filesystem::path normalized = std::filesystem::weakly_canonical(destination, canonicalError);
        if (canonicalError) {
            LOG_ERROR("editor.scene_document", "Scene destination '%s' could not be normalized: %s", destination.string().c_str(),
                      canonicalError.message().c_str());
            return true;
        }

        EditorWorkspaceViewCommandData command;
        command.command = copyOnly ? EditorWorkspaceViewCommand::SaveSceneCopyAs : EditorWorkspaceViewCommand::SaveSceneAs;
        command.stringPayload = normalized.string();
        controller_->ProcessCommand(command);
        PublishViewportSceneIfChanged();
        return true;
    }

    /** @copydoc WorkspaceSourceDocumentCommands::OpenCloseModal */
    bool WorkspaceSourceDocumentCommands::OpenCloseModal(EditorWorkspaceController &controller,
                                                         const EditorWorkspaceViewCommandData &command, EditorModalHost &modalHost,
                                                         const EditorGuiContext &context) {
        if (!command.documentInstance)
            return false;
        const auto snapshot = controller.SourceDocuments().Snapshot(*command.documentInstance);
        if (snapshot.HasError() || !snapshot.Value().Dirty())
            return false;
        static_cast<void>(modalHost.OpenRoot(
            std::make_unique<SourceDocumentCloseModal>(context, controller, closeLifetime_,
                                                       SourceSaveRequest{*command.documentInstance, snapshot.Value().Revision(), {}},
                                                       snapshot.Value().Identity().key.source.Value())));
        return true;
    }

    /** @copydoc EditorWorkspaceScreen::OpenPackagesModal */
    void EditorWorkspaceScreen::OpenPackagesModal() {
        if (!controller_ || !packages_.acquire)
            return;
        const auto acquire = packages_.acquire;
        const std::filesystem::path root{controller_->ViewModel().projectRoot};
        const auto opened = modalHost_.OpenRoot(std::make_unique<PackageModal>(context_, [acquire, root] {
            return acquire(root);
        }));
        if (opened.HasError())
            LOG_WARN("editor.packages", "Package modal admission failed: %s", opened.ErrorValue().code.Value().c_str());
    }

    /** @copydoc EditorWorkspaceScreen::OpenPlayTopologyProfilesModal */
    void EditorWorkspaceScreen::OpenPlayTopologyProfilesModal() {
        if (!controller_ || !projectServices_.durableFiles)
            return;
        const auto &model = controller_->ViewModel();
        if (model.playState != EditorPlayState::Idle && model.playState != EditorPlayState::Failed)
            return;
        const std::filesystem::path root{model.projectRoot};
        auto store =
            std::make_unique<Application::PlayTopologyStore>(*projectServices_.durableFiles, root / "config" / "play_topologies.json",
                                                             root / ".horo" / "local" / "play_topology_overrides.json");
        const auto opened = modalHost_.OpenRoot(std::make_unique<PlayTopologyModal>(context_, std::move(store)));
        if (opened.HasError())
            LOG_WARN("editor.play_topology", "Profile modal admission failed: %s", opened.ErrorValue().message.c_str());
    }

    /** @copydoc EditorWorkspaceScreen::OpenSceneComparison */
    void EditorWorkspaceScreen::OpenSceneComparison() {
        Result<SceneDocumentComparisonRequest> captured = controller_->CaptureExternalSceneComparison();
        if (captured.HasError()) {
            LOG_ERROR("editor.scene_document", "External scene comparison failed: %s", captured.ErrorValue().message.c_str());
            return;
        }
        Result<void> opened = modalHost_.OpenRoot(
            std::make_unique<SceneConflictCompareModal>(context_, services_.Get<JobSystem>(), std::move(captured).Value()));
        if (opened.HasError()) {
            LOG_WARN("editor.scene_document", "External scene comparison modal could not open: %s", opened.ErrorValue().message.c_str());
        }
    }

    /** @copydoc EditorWorkspaceScreen::OpenWorldLayerCreation */
    void EditorWorkspaceScreen::OpenWorldLayerCreation(const WorldDocumentFence &expected) {
        const auto opened = modalHost_.OpenRoot(
            std::make_unique<CreateWorldLayerModal>(context_, expected, [this](std::string name, const WorldDocumentFence &fence) {
            if (!controller_)
                return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(WorldDocumentErrors::Lifecycle));
            return controller_->WorldDocuments().CreateLayer(std::move(name), fence);
        }));
        if (opened.HasError() && controller_)
            controller_->Notifications().Publish("world_layers", NotificationSeverity::Error,
                                                 context_.localization.Get("editor", "workspace.world_layers.rejected"));
    }

    /** @copydoc EditorWorkspaceScreen::OpenPrefabCreationModal */
    void EditorWorkspaceScreen::OpenPrefabCreationModal(const SceneObjectId root) {
        const auto captured = controller_->CapturePrefabCreationIntent(root);
        if (captured.HasError())
            return;
        auto opened = modalHost_.OpenRoot(
            std::make_unique<PrefabCreationModal>(context_, captured.Value(), controller_->ViewModel().projectRoot,
                                                  [controller = controller_.get(), localization = &context_.localization,
                                                   lifetime = std::weak_ptr<const void>{workspaceControllerLifetime_},
                                                   expired = context_.localization.Get("editor", "workspace.prefab.create.expired")](
                                                      PrefabCreationIntent intent) -> std::optional<std::string> {
            if (lifetime.expired())
                return expired;
            if (const auto admission = controller->CapturePrefabCreationIntent(intent.root);
                admission.HasError() || admission.Value().documentRevision != intent.documentRevision ||
                admission.Value().replacementEpoch != intent.replacementEpoch)
                return expired;
            EditorWorkspaceViewCommandData command;
            command.command = EditorWorkspaceViewCommand::CreatePrefabFromSelection;
            command.prefabCreation = std::move(intent);
            controller->ProcessCommand(command);
            const auto &outcome = controller->ViewModel().prefabCreationOutcome;
            if (outcome && outcome->committed)
                return {};
            return PrefabCreationFailureMessage(outcome.get(), *localization);
        }));
        if (opened.HasError())
            LOG_WARN("editor.prefab", "Creation modal could not open: %s", opened.ErrorValue().message.c_str());
    }

    /** @copydoc EditorWorkspaceScreen::OpenGameplayBehaviorModal */
    void EditorWorkspaceScreen::OpenGameplayBehaviorModal(const GameplayBehaviorKind kind, const std::string &requestedDirectory) {
        if (!controller_) {
            return;
        }
        const std::optional<std::filesystem::path> destination =
            ResolveGameplayBehaviorDestination(controller_->ViewModel().projectRoot, kind, requestedDirectory);
        if (!destination.has_value()) {
            EditorWorkspaceViewCommandData fallback;
            fallback.command = kind == GameplayBehaviorKind::Native ? EditorWorkspaceViewCommand::CreateNativeBehavior
                                                                    : EditorWorkspaceViewCommand::CreateLuaBehavior;
            fallback.stringPayload = requestedDirectory;
            controller_->ProcessCommand(fallback);
            return;
        }

        const std::string baseName = SuggestGameplayBehaviorBaseName(*destination, kind);
        Result<void> opened =
            modalHost_.OpenRoot(std::make_unique<GameplayBehaviorFilenameModal>(context_, kind, destination->string(), baseName,
                                                                                [this](CreateGameplayBehaviorRequest request) {
            if (!controller_) {
                return;
            }
            EditorWorkspaceViewCommandData create;
            create.command = request.kind == GameplayBehaviorKind::Native ? EditorWorkspaceViewCommand::CreateNativeBehavior
                                                                          : EditorWorkspaceViewCommand::CreateLuaBehavior;
            create.gameplayBehaviorRequest = std::move(request);
            controller_->ProcessCommand(create);
            PublishViewportSceneIfChanged();
        }));
        if (opened.HasError()) {
            LOG_WARN("editor.asset_actions", "Gameplay behavior filename modal could not open: %s", opened.ErrorValue().message.c_str());
        }
    }

}  // namespace Horo::Editor::WorkspaceScreenDetail
