#include "EditorWorkspaceScreen.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/screens/NavigationErrors.h"

namespace Horo::Editor::WorkspaceScreenDetail {
    /** @copydoc EditorWorkspaceScreen::CanLeave */
    LeaveDecision EditorWorkspaceScreen::CanLeave(const LeaveTarget &) const {
        using enum LeaveAction;
        using enum LeaveDisposition;
        if (HasDirtyDocuments()) {
            static_cast<void>(controller_->HasDirtySourceDocuments());
            leaveCapture_.sourceDocuments = controller_->ViewModel().sourceDocuments;
            auto graphs = controller_->PCGDocuments().Snapshots();
            if (graphs.HasError())
                return LeaveDecision{.disposition = Deny, .requirement = std::nullopt};
            leaveCapture_.pcgDocuments = std::move(graphs).Value();
            if (const auto collision = CaptureLeaveCollision(); collision.HasError())
                return LeaveDecision{.disposition = Deny, .requirement = std::nullopt};
            const auto world = controller_->WorldDocuments().Snapshot();
            leaveCapture_.worldFence = world.HasValue() ? std::optional{world.Value()->fence} : std::nullopt;
            LeaveRequirement requirement{.kind = LeaveRequirementKind::DirtyDocument,
                                         .subject = 1,
                                         .revision = 1,
                                         .allowedActions = controller_->HasDirtyWorldDocuments() ? std::vector{Discard, Stay}
                                                                                                 : std::vector{Save, Discard, Stay}};
            return LeaveDecision{.disposition = RequireResolution, .requirement = requirement};
        }
        return LeaveDecision{.disposition = Allow, .requirement = std::nullopt};
    }

    /** @copydoc EditorWorkspaceScreen::SaveForLeave */
    Result<LeaveDecision> EditorWorkspaceScreen::SaveForLeave() {
        using enum LeaveDisposition;
        if (!controller_)
            return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
        if (controller_->HasDirtyWorldDocuments())
            return Result<LeaveDecision>::Failure(MakeError(WorldDocumentErrors::Dirty));
        if (const auto navigation = controller_->SaveNavigationDocuments(); navigation.HasError())
            return Result<LeaveDecision>::Failure(navigation.ErrorValue());
        if (const auto sources = controller_->SaveSourceDocuments(); sources.HasError())
            return Result<LeaveDecision>::Failure(sources.ErrorValue());
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::SaveScene;
        controller_->ProcessCommand(command);
        if (controller_->ViewModel().isDirty || controller_->SaveCollisionSettings().HasError() ||
            controller_->SavePCGDocuments().HasError() || controller_->HasDirtyPCGDocuments())
            return Result<LeaveDecision>::Success({.disposition = Deny, .requirement = std::nullopt});
        if (const auto navigation = controller_->ShutdownNavigationDocuments(); navigation.HasError())
            return Result<LeaveDecision>::Failure(navigation.ErrorValue());
        if (const auto graphs = controller_->ShutdownPCGDocuments(); graphs.HasError())
            return Result<LeaveDecision>::Failure(graphs.ErrorValue());
        return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
    }

    /** @copydoc EditorWorkspaceScreen::DiscardForLeave */
    Result<LeaveDecision> EditorWorkspaceScreen::DiscardForLeave() {
        using enum LeaveDisposition;
        if (!controller_)
            return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::DiscardSceneRecovery;
        controller_->ProcessCommand(command);
        if (const auto collision = controller_->ShutdownCollisionSettings(true); collision.HasError())
            return Result<LeaveDecision>::Failure(collision.ErrorValue());
        if (const auto navigation = controller_->ShutdownNavigationDocuments(true); navigation.HasError())
            return Result<LeaveDecision>::Failure(navigation.ErrorValue());
        if (const auto current = ValidateLeaveWorld(); current.HasError())
            return Result<LeaveDecision>::Failure(current.ErrorValue());
        if (const auto world = controller_->WorldDocuments().Shutdown(WorldDocumentDisposition::Discard); world.HasError())
            return Result<LeaveDecision>::Failure(world.ErrorValue());
        if (const auto graphs = controller_->ShutdownPCGDocuments(true); graphs.HasError())
            return Result<LeaveDecision>::Failure(graphs.ErrorValue());
        return Result<LeaveDecision>::Success({.disposition = Allow, .requirement = std::nullopt});
    }

    /** @copydoc EditorWorkspaceScreen::ResolveLeave */
    Result<LeaveDecision> EditorWorkspaceScreen::ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) {
        using enum LeaveAction;
        using enum LeaveDisposition;
        if (resolution.subject != 1 || resolution.revision != 1)
            return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        if (controller_ && resolution.action != Stay) {
            static_cast<void>(controller_->HasDirtySourceDocuments());
            if (controller_->ViewModel().sourceDocuments.size() != leaveCapture_.sourceDocuments.size())
                return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
            for (const auto &captured : leaveCapture_.sourceDocuments) {
                const auto current = controller_->SourceDocuments().Snapshot(captured.Identity().instance);
                if (current.HasError() || current.Value().Revision() != captured.Revision() ||
                    current.Value().Identity() != captured.Identity())
                    return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
            }
            if (const auto current = ValidateLeaveCapture(); current.HasError())
                return Result<LeaveDecision>::Failure(current.ErrorValue());
        }
        switch (resolution.action) {
            case Save:
                return SaveForLeave();
            case Discard:
                return DiscardForLeave();
            case Stay:
                return Result<LeaveDecision>::Success({.disposition = Deny, .requirement = std::nullopt});
            default:
                return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::WorkspaceLeaveActionNotAllowed));
        }
    }

    /** @copydoc EditorWorkspaceScreen::OnLeave */
    void EditorWorkspaceScreen::OnLeave() {
        view_.ResetSourceSession();
        sourceCommands_.EndSession();
        LOG_INFO("editor.workspace", "EditorWorkspaceScreen leaving.");
        if (controller_) {
            static_cast<void>(controller_->ShutdownCollisionSettings(true));
            if (packages_.retire)
                static_cast<void>(packages_.retire(std::filesystem::path{controller_->ViewModel().projectRoot}));
            SaveExtensionWorkspace();
            controller_->FlushAutosave();
            if (const auto gameplay = controller_->ShutdownGameplayModules(); gameplay.HasError())
                LOG_WARN("editor.gameplay", "Gameplay retirement remains owned by the application: %s",
                         gameplay.ErrorValue().message.c_str());
            if (const auto navigation = controller_->ShutdownNavigationDocuments(); navigation.HasError())
                LOG_ERROR("editor.navigation_document", "Navigation document shutdown is incomplete: %s",
                          navigation.ErrorValue().message.c_str());
            if (const auto world = controller_->WorldDocuments().Shutdown(); world.HasError())
                LOG_ERROR("editor.world_document", "World shutdown requires an explicit dirty decision: %s",
                          world.ErrorValue().message.c_str());
            if (const auto graphs = controller_->ShutdownPCGDocuments(); graphs.HasError())
                LOG_ERROR("editor.pcg_document", "Graph shutdown recovery is incomplete: %s", graphs.ErrorValue().message.c_str());
        }
        static_cast<void>(contributions_.statusItems.Update("horo.status.document", EditorStatusItemContent{.available = false}));
        static_cast<void>(contributions_.statusItems.Update("horo.status.selection", EditorStatusItemContent{.available = false}));
        contributions_.panels.DetachAll();
        if (input_.previousProfile.has_value()) {
            if (const Result<void> restored = input_.router.SetProfile(std::move(*input_.previousProfile)); restored.HasError()) {
                LOG_ERROR("editor.input", "Unable to restore editor input profile: %s", restored.ErrorValue().message.c_str());
            }
            input_.previousProfile.reset();
        }
        input_.context.Reset();
        viewportSceneState_.Clear();
        publishedRevisions_ = {};
        controller_.reset();
    }

    /** @copydoc EditorWorkspaceScreen::HasDirtyDocuments */
    bool EditorWorkspaceScreen::HasDirtyDocuments() const {
        return controller_ && (controller_->ViewModel().isDirty || controller_->HasDirtyPCGDocuments() ||
                               controller_->HasDirtyCollisionSettings() || controller_->HasDirtyNavigationDocuments() ||
                               controller_->HasDirtyWorldDocuments() || controller_->HasDirtySourceDocuments());
    }

    /** @copydoc EditorWorkspaceScreen::ValidateLeaveCapture */
    Result<void> EditorWorkspaceScreen::ValidateLeaveCapture() const {
        if (const auto current = ValidateLeaveWorld(); current.HasError())
            return current;
        if (const auto current = ValidateLeavePCGDocuments(); current.HasError())
            return current;
        return ValidateLeaveCollision();
    }

    /** @copydoc EditorWorkspaceScreen::ValidateLeaveWorld */
    Result<void> EditorWorkspaceScreen::ValidateLeaveWorld() const {
        const auto world = controller_->WorldDocuments().Snapshot();
        if (const auto fence = world.HasValue() ? std::optional{world.Value()->fence} : std::nullopt; fence != leaveCapture_.worldFence)
            return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::CaptureLeaveCollision */
    Result<void> EditorWorkspaceScreen::CaptureLeaveCollision() const {
        leaveCapture_.collisionOwner.reset();
        leaveCapture_.collisionRoot.reset();
        if (!controller_->HasDirtyCollisionSettings())
            return Result<void>::Success();
        auto owner = controller_->CollisionSettings();
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        auto root = owner.Value()->Snapshot();
        if (root.HasError())
            return Result<void>::Failure(root.ErrorValue());
        leaveCapture_.collisionOwner = owner.Value();
        leaveCapture_.collisionRoot = std::move(root).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::ValidateLeaveCollision */
    Result<void> EditorWorkspaceScreen::ValidateLeaveCollision() const {
        if (controller_->HasDirtyCollisionSettings() != static_cast<bool>(leaveCapture_.collisionRoot))
            return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        if (!leaveCapture_.collisionRoot)
            return Result<void>::Success();
        const auto owner = leaveCapture_.collisionOwner.lock();
        if (const auto currentOwner = controller_->CollisionSettings(); !owner || currentOwner.HasError() || currentOwner.Value() != owner)
            return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        if (const auto snapshot = owner->Snapshot(); snapshot.HasError() || snapshot.Value() != leaveCapture_.collisionRoot)
            return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceScreen::ValidateLeavePCGDocuments */
    Result<void> EditorWorkspaceScreen::ValidateLeavePCGDocuments() const {
        const auto graphs = controller_->PCGDocuments().Snapshots();
        if (graphs.HasError())
            return Result<void>::Failure(graphs.ErrorValue());
        if (graphs.Value().size() != leaveCapture_.pcgDocuments.size())
            return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        for (std::size_t index = 0; index < graphs.Value().size(); ++index) {
            const auto &current = graphs.Value()[index];
            const auto &captured = leaveCapture_.pcgDocuments[index];
            if (current.session != captured.session || current.revision != captured.revision ||
                current.catalogRevision != captured.catalogRevision)
                return Result<void>::Failure(MakeError(NavigationErrors::WorkspaceStaleLeaveSubject));
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Editor::WorkspaceScreenDetail
