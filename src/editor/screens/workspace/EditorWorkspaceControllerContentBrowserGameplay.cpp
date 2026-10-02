#include "Horo/Editor/ProjectIntegrityValidatorService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Paths.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceControllerContentBrowserInternal.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <filesystem>
#include <format>
#include <memory>
#include <string>

namespace Horo::Editor {

    void EditorWorkspaceController::CreateGameplayBehavior(const CreateGameplayBehaviorRequest &request) {
        m_viewModel.contentBrowserOperationError.clear();
        if (const Result<void> validation = ValidateCreateGameplayBehaviorRequest(request); validation.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_name";
            return;
        }
        const bool nativeBehavior = request.kind == GameplayBehaviorKind::Native;
        LOG_INFO("editor.asset_actions", "Create %s behavior requested: directory='%s' base='%s'", nativeBehavior ? "native" : "lua",
                 request.destination.c_str(), request.baseName.c_str());
        if (m_mutations == nullptr || m_durableFiles == nullptr) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.unavailable";
            return;
        }

        const std::filesystem::path projectRoot = NormalizeAbsolute(m_viewModel.projectRoot);
        const std::filesystem::path assetsRoot = ProjectLayout::AssetRoot(projectRoot);
        const std::filesystem::path scriptsRoot = ProjectLayout::ScriptsRoot(projectRoot);
        const std::filesystem::path requestedDirectory = NormalizeAbsolute(request.destination);
        std::filesystem::path directory;
        if (nativeBehavior)
            directory = projectRoot / "source" / "gameplay";
        else
            directory = HasPathPrefix(scriptsRoot, requestedDirectory) ? requestedDirectory : scriptsRoot;
        if ((!nativeBehavior && !HasPathPrefix(assetsRoot, directory)) || (nativeBehavior && !HasPathPrefix(projectRoot, directory))) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.invalid_target";
            return;
        }

        CreateGameplayBehaviorFiles(request, nativeBehavior, projectRoot, directory);
    }

    void EditorWorkspaceController::CreateGameplayBehaviorFiles(const CreateGameplayBehaviorRequest &request, const bool nativeBehavior,
                                                                const std::filesystem::path &projectRoot,
                                                                const std::filesystem::path &directory) {
        if (auto lease = m_mutations->TryAcquire(ProjectMutationRequest{
                .projectRoot = projectRoot,
                .owner = ProjectMutationOwner::Asset,
                .operationId = nativeBehavior ? "create-native-behavior" : "create-lua-behavior",
            });
            lease.HasError()) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.busy";
            return;
        } else {
            WriteGameplayBehaviorSource(request, nativeBehavior, projectRoot, directory);
        }
    }

    void EditorWorkspaceController::WriteGameplayBehaviorSource(const CreateGameplayBehaviorRequest &request, const bool nativeBehavior,
                                                                const std::filesystem::path &projectRoot,
                                                                const std::filesystem::path &directory) {
        std::error_code filesystemError;
        std::filesystem::create_directories(directory, filesystemError);
        if (filesystemError) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_behavior_failed";
            return;
        }

        const std::filesystem::path source = directory / (request.baseName + (nativeBehavior ? ".cpp" : ".horo_script"));
        if (std::filesystem::exists(source, filesystemError) || filesystemError) {
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.name_exists";
            return;
        }

        const std::string behaviorName = source.stem().string();
        const std::string typeId =
            std::format("game.{}.{}.{}", BehaviorNamespace(projectRoot), nativeBehavior ? "cpp" : "lua", BehaviorSlug(behaviorName));
        const std::string contents =
            nativeBehavior ? NativeBehaviorContents(behaviorName, typeId) : LuaBehaviorContents(behaviorName, typeId);
        if (const Result<void> written = WriteBehaviorFiles(*m_durableFiles, source, contents, nativeBehavior, typeId);
            written.HasError()) {
            LOG_ERROR("editor.asset_actions", "Create %s behavior files failed for '%s': %s", nativeBehavior ? "native" : "lua",
                      source.string().c_str(), written.ErrorValue().message.c_str());
            m_viewModel.contentBrowserOperationError = "workspace.content_browser.operation.create_behavior_failed";
            return;
        }
        if (nativeBehavior) {
            ProjectIntegrityValidatorService validator{*m_durableFiles};
            static_cast<void>(validator.Repair(projectRoot));
        }

        static_cast<void>(m_durableFiles->SyncDirectory(directory));
        RefreshContentBrowserAfterMutation();
        if (nativeBehavior)
            m_nativeBuildDebounceSeconds = 0.25F;
        else
            RefreshGameplayRegistry();
        LOG_INFO("editor.asset_actions", "Create %s behavior completed: source='%s'", nativeBehavior ? "native" : "lua",
                 source.string().c_str());
    }

    void EditorWorkspaceController::RefreshGameplayRegistry() {
        if (m_nativeGameplayReloadQuarantined)
            return;
        if (m_playSession.IsActive() && m_gameplayRegistry && m_gameplayRegistry->HasNativeModule()) {
            m_nativeGameplayReloadPending = true;
            return;
        }
        std::unique_ptr<ProjectGameplayRegistry> candidate = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (m_playSession.IsActive()) {
            m_pendingGameplayRegistry = std::move(candidate);
            return;
        }
        m_gameplayRegistry = std::move(candidate);
        RefreshAvailableBehaviorProjection();
    }

    void EditorWorkspaceController::RefreshAvailableBehaviorProjection() {
        m_viewModel.availableBehaviors.clear();
        for (const Gameplay::BehaviorRegistration &registration : m_gameplayRegistry->Registry().Registrations())
            m_viewModel.availableBehaviors.push_back(registration.descriptor);
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->Diagnostics())
            LOG_ERROR("editor.gameplay", "Gameplay source '%s' is invalid: %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
    }

    void EditorWorkspaceController::ApplyPendingGameplayRegistry() {
        if (!m_pendingGameplayRegistry)
            return;
        if (m_pendingGameplayRegistry->HasBlockingDiagnostics()) {
            for (const ProjectGameplayDiagnostic &diagnostic : m_pendingGameplayRegistry->Diagnostics())
                LOG_ERROR("editor.gameplay", "Gameplay registry candidate was rejected for '%s': %s", diagnostic.source.string().c_str(),
                          diagnostic.error.message.c_str());
            m_pendingGameplayRegistry.reset();
            return;
        }
        if (const Result<void> reloaded =
                m_playSession.ReloadBehaviors(m_pendingGameplayRegistry->Registry(), m_gameplayRegistry->Registry());
            reloaded.HasError()) {
            LOG_ERROR("editor.gameplay", "Gameplay reload rolled back to the previous module: %s", reloaded.ErrorValue().message.c_str());
            m_pendingGameplayRegistry.reset();
            RefreshPlayStateProjection();
            return;
        }
        m_gameplayRegistry = std::move(m_pendingGameplayRegistry);
        RefreshAvailableBehaviorProjection();
        LOG_INFO("editor.gameplay", "Gameplay module reloaded at a fixed-tick safe point.");
    }

    void EditorWorkspaceController::ApplyNativeGameplayReload() {
        if (m_nativeGameplayReloadQuarantined || !m_nativeGameplayReloadPending)
            return;
        m_nativeGameplayReloadPending = false;
        m_pendingGameplayRegistry.reset();
        if (!m_gameplayRegistry || !m_gameplayRegistry->HasNativeModule() || !m_playSession.IsActive())
            return;

        const std::filesystem::path projectRoot{m_viewModel.projectRoot};
        auto begun = BeginNativeGameplayReload(projectRoot);
        if (begun.HasError()) {
            LogNativeReloadFailure("could not begin safely", begun.ErrorValue());
            return;
        }
        NativeGameplayReloadTransaction transaction = std::move(begun).Value();
        if (Result<void> retired = RetireNativeGameplayGeneration(transaction); retired.HasError()) {
            Error error = retired.ErrorValue();
            DegradeNativeGameplayReload(transaction, error);
            LogNativeReloadFailure("could not retire safely", error);
            return;
        }
        m_gameplayRegistry.reset();
        auto candidate = TryActivateNativeGameplayGeneration(projectRoot, transaction);
        if (candidate.HasValue()) {
            CommitNativeGameplayReload(std::move(candidate).Value(), transaction);
            return;
        }
        RollbackNativeGameplayReload(projectRoot, std::move(transaction), candidate.ErrorValue());
    }

    Result<EditorWorkspaceController::NativeGameplayReloadTransaction> EditorWorkspaceController::BeginNativeGameplayReload(
        const std::filesystem::path &projectRoot) const {
        auto preserved =
            m_gameplayRegistry->PreserveNativeArtifactForRollback(projectRoot / ".horo" / "local" / "gameplay_module_rollback");
        if (preserved.HasError())
            return Result<NativeGameplayReloadTransaction>::Failure(preserved.ErrorValue());
        auto luaGeneration = m_gameplayRegistry->CaptureLuaGeneration();
        if (luaGeneration.HasError())
            return Result<NativeGameplayReloadTransaction>::Failure(luaGeneration.ErrorValue());
        NativeGameplayReloadTransaction transaction;
        transaction.rollbackArtifact = std::move(preserved).Value();
        transaction.luaGeneration = std::move(luaGeneration).Value();
        return Result<NativeGameplayReloadTransaction>::Success(std::move(transaction));
    }

    Result<void> EditorWorkspaceController::RetireNativeGameplayGeneration(NativeGameplayReloadTransaction &transaction) {
        auto playSnapshot = m_playSession.QuiesceForReload();
        if (playSnapshot.HasError())
            return Result<void>::Failure(playSnapshot.ErrorValue());
        transaction.playSnapshot = std::move(playSnapshot).Value();
        auto moduleSnapshot = m_gameplayRegistry->PrepareNativeReload();
        if (moduleSnapshot.HasError())
            return Result<void>::Failure(moduleSnapshot.ErrorValue());
        transaction.moduleSnapshot = std::move(moduleSnapshot).Value();
        transaction.phase = NativeGameplayReloadTransaction::Phase::Retired;
        return Result<void>::Success();
    }

    Result<std::unique_ptr<ProjectGameplayRegistry>> EditorWorkspaceController::TryActivateNativeGameplayGeneration(
        const std::filesystem::path &projectRoot, NativeGameplayReloadTransaction &transaction) {
        if (transaction.phase != NativeGameplayReloadTransaction::Phase::Retired)
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(
                MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed, "Native reload generation was not retired."));
        std::unique_ptr<ProjectGameplayRegistry> candidate =
            ProjectGameplayRegistry::DiscoverNativeGeneration(projectRoot, transaction.luaGeneration);
        if (!candidate->HasNativeModule() || candidate->HasBlockingDiagnostics())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(
                NativeGenerationError(*candidate, "The replacement native gameplay generation could not be loaded."));
        if (Result<void> restored = candidate->RestoreNativeReload(transaction.moduleSnapshot); restored.HasError())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(restored.ErrorValue());
        if (Result<void> restored = m_playSession.RestoreAfterReload(candidate->Registry(), transaction.playSnapshot); restored.HasError())
            return Result<std::unique_ptr<ProjectGameplayRegistry>>::Failure(restored.ErrorValue());
        transaction.phase = NativeGameplayReloadTransaction::Phase::Activated;
        return Result<std::unique_ptr<ProjectGameplayRegistry>>::Success(std::move(candidate));
    }

    void EditorWorkspaceController::CommitNativeGameplayReload(std::unique_ptr<ProjectGameplayRegistry> generation,
                                                               NativeGameplayReloadTransaction &transaction) {
        m_gameplayRegistry = std::move(generation);
        transaction.phase = NativeGameplayReloadTransaction::Phase::Committed;
        RefreshAvailableBehaviorProjection();
        LOG_INFO("editor.gameplay", "Native gameplay generation reloaded transactionally at a fixed-tick safe point.");
    }

    void EditorWorkspaceController::RollbackNativeGameplayReload(const std::filesystem::path &projectRoot,
                                                                 NativeGameplayReloadTransaction transaction, const Error &candidateError) {
        std::unique_ptr<ProjectGameplayRegistry> rollback =
            ProjectGameplayRegistry::DiscoverRollback(projectRoot, transaction.rollbackArtifact, transaction.luaGeneration);
        Error rollbackError = NativeGenerationError(*rollback, "The previous native gameplay generation could not be loaded.");
        const bool ready = rollback->HasNativeModule() && !rollback->HasBlockingDiagnostics();
        Result<void> activated =
            ready ? rollback->RestoreNativeReload(transaction.moduleSnapshot) : Result<void>::Failure(std::move(rollbackError));
        if (activated.HasValue())
            activated = m_playSession.RestoreAfterReload(rollback->Registry(), transaction.playSnapshot);
        if (activated.HasValue()) {
            m_gameplayRegistry = std::move(rollback);
            transaction.phase = NativeGameplayReloadTransaction::Phase::RolledBack;
            RefreshAvailableBehaviorProjection();
            LogNativeReloadFailure("rejected the candidate and restored the previous generation", candidateError);
            return;
        }
        rollback.reset();
        rollbackError = activated.ErrorValue();
        DegradeNativeGameplayReload(transaction, rollbackError);
        LogNativeReloadFailure("could not restore the candidate", candidateError);
        LogNativeReloadFailure("could not restore the previous generation; Play Mode was stopped", rollbackError);
    }

    void EditorWorkspaceController::DegradeNativeGameplayReload(NativeGameplayReloadTransaction &transaction, Error error) {
        transaction.phase = NativeGameplayReloadTransaction::Phase::Degraded;
        m_nativeGameplayReloadQuarantined = true;
        m_nativeGameplayReloadPending = false;
        m_pendingGameplayRegistry.reset();
        if (m_playSession.State() == EditorPlaySessionState::Reloading)
            m_playSession.DegradeAfterReload(std::move(error));
    }

}  // namespace Horo::Editor
