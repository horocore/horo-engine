#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Assets/AssetRegistry.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error Unavailable() {
            return MakeError(CollisionSettingsError(CollisionSettingsFailure::Unsupported),
                             "The project host must supply complete registry and explicit collision activation evidence.");
        }
    }  // namespace

    Result<void> EditorWorkspaceController::BindCollisionScene() {
        if (m_collisionScene.has_value())
            return Result<void>::Success();
        if (!m_collisionSceneOwner.IsValid() || m_collisionSceneGeneration == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(Unavailable());
        auto port = SceneDocumentTransactions::Bind(m_document, m_history, {m_collisionSceneOwner, ++m_collisionSceneGeneration});
        if (port.HasError())
            return Result<void>::Failure(port.ErrorValue());
        m_collisionScene.emplace(std::move(port).Value());
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::CollisionSettings */
    Result<std::shared_ptr<CollisionProjectSettingsSession>> EditorWorkspaceController::CollisionSettings() {
        using Opening = Result<std::shared_ptr<CollisionProjectSettingsSession>>;
        if (m_collisionSettings)
            return Opening::Success(m_collisionSettings);
        if (!m_mutableAssetRegistry || !m_durableFiles || !m_mutations || !m_collisionInventory || !m_collisionActivation ||
            !m_defaultScenePath.has_value() || !m_assetRegistrySource)
            return Opening::Failure(Unavailable());
        auto metadata = Application::LoadProjectMetadata(std::filesystem::path{m_viewModel.projectRoot});
        if (metadata.HasError())
            return Opening::Failure(metadata.ErrorValue());
        if (auto bound = BindCollisionScene(); bound.HasError())
            return Opening::Failure(bound.ErrorValue());
        RefreshCollisionActivation();
        const CollisionProjectSettingsOwners owners{*m_mutableAssetRegistry, *m_durableFiles,    *m_mutations,        *m_collisionInventory,
                                                    *m_assetRegistrySource,  &*m_collisionScene, *m_defaultScenePath, m_triggerBinding};
        auto opened = CollisionProjectSettingsSession::Open(std::filesystem::path{m_viewModel.projectRoot}, metadata.Value().projectId,
                                                            owners, *m_collisionActivation);
        if (opened.HasError())
            return opened;
        m_collisionSettings = opened.Value();
        return opened;
    }

    /** @copydoc EditorWorkspaceController::HasDirtyCollisionSettings */
    bool EditorWorkspaceController::HasDirtyCollisionSettings() const noexcept {
        return m_collisionSettings && m_collisionSettings->IsDirty();
    }

    /** @copydoc EditorWorkspaceController::SaveCollisionSettings */
    Result<void> EditorWorkspaceController::SaveCollisionSettings() {
        if (!HasDirtyCollisionSettings())
            return Result<void>::Success();
        auto saved = m_collisionSettings->Save();
        if (saved.HasError())
            return Result<void>::Failure(saved.ErrorValue());
        if (saved.Value().durabilityError)
            return Result<void>::Failure(*saved.Value().durabilityError);
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ShutdownCollisionSettings */
    Result<void> EditorWorkspaceController::ShutdownCollisionSettings(const bool discardDirty) {
        if (!discardDirty && HasDirtyCollisionSettings())
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::InUse),
                                                   "Save or explicitly discard collision settings before replacing the Scene owner."));
        if (m_collisionSettings)
            m_collisionSettings->Close();
        m_collisionSettings.reset();
        if (m_collisionScene)
            m_collisionScene->Shutdown();
        m_collisionScene.reset();
        return Result<void>::Success();
    }

    void EditorWorkspaceController::RefreshCollisionActivation() {
        if (!m_collisionActivation)
            return;
        const bool running =
            m_playSession.State() != EditorPlaySessionState::Idle && m_playSession.State() != EditorPlaySessionState::Failed;
        if (m_collisionActivation->playRunning != running) {
            if (m_collisionActivation->revision == std::numeric_limits<std::uint64_t>::max()) {
                m_collisionActivation->state = CollisionActivationState::Unknown;
                m_collisionActivation->documentRevision = 0;
                m_collisionActivation->generation = 0;
                m_collisionActivation->fingerprint = 0;
            } else {
                ++m_collisionActivation->revision;
                m_collisionActivation->playRunning = running;
            }
        }
        if (m_collisionSettings) {
            const auto previous = m_collisionSettings->Activation();
            if (!previous.HasError() && *previous.Value() != *m_collisionActivation)
                static_cast<void>(m_collisionSettings->ObserveActivation(*m_collisionActivation));
        }
    }

    Result<Assets::AssetRegistryBuildReport> EditorWorkspaceController::RebuildProjectAssetRegistry(const std::filesystem::path &root) {
        if (m_collisionInventory)
            *m_collisionInventory = {};
        if (m_collisionSettings)
            static_cast<void>(m_collisionSettings->ObserveInventory({}));
        auto rebuilt = Assets::RebuildAssetRegistry(*m_mutableAssetRegistry, root, Assets::AssetRegistryOpenMode::Edit);
        if (rebuilt.HasError())
            return rebuilt;
        const CollisionInventoryEvidence evidence{rebuilt.Value().publishedRevision, rebuilt.Value().status};
        if (m_collisionInventory)
            *m_collisionInventory = evidence;
        if (m_collisionSettings)
            static_cast<void>(m_collisionSettings->ObserveInventory(evidence));
        return rebuilt;
    }
}  // namespace Horo::Editor
