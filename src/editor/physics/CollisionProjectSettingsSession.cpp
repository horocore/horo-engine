#include "CollisionIdentityGeneration.h"
#include "CollisionProjectSettingsSessionState.h"

#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Validates explicit host evidence without inferring runtime state from service absence. */
        bool ValidActivation(const CollisionActivationEvidence &evidence, const std::string_view project) {
            if (evidence.revision == 0 || static_cast<unsigned>(evidence.state) > static_cast<unsigned>(CollisionActivationState::Active))
                return false;
            if (evidence.state != CollisionActivationState::Active)
                return evidence.generation == 0 && evidence.fingerprint == 0 && evidence.documentRevision == 0;
            return evidence.projectId == project && evidence.generation != 0 && evidence.fingerprint != 0 && evidence.documentRevision != 0;
        }
    }  // namespace

    /** @copydoc CollisionProjectSettingsSession::State::Admit */
    Result<void> CollisionProjectSettingsSession::State::Admit() const {
        if (owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::WrongThread)));
        if (closed)
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Closed)));
        return Result<void>::Success();
    }

    /** @copydoc CollisionProjectSettingsSession::Open */
    Result<std::shared_ptr<CollisionProjectSettingsSession>> CollisionProjectSettingsSession::Open(
        const std::filesystem::path &root, std::string projectId, const CollisionProjectSettingsOwners &owners,
        CollisionActivationEvidence activation) {
        using Return = Result<std::shared_ptr<CollisionProjectSettingsSession>>;
        try {
            if (!ValidActivation(activation, projectId) || (owners.activeScene && owners.activeScenePath.empty()) ||
                static_cast<unsigned>(owners.inventory.status) > static_cast<unsigned>(Assets::AssetRegistryBuildStatus::Failed))
                return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
            auto loaded = Application::LoadProjectCollisionSettings(root, projectId);
            if (loaded.HasError())
                return Return::Failure(loaded.ErrorValue());
            auto state = std::make_unique<State>();
            state->root = root;
            state->projectId = std::move(projectId);
            state->authorities.registry = &owners.registry;
            state->authorities.source = &owners.source;
            state->authorities.files = &owners.files;
            state->authorities.mutations = &owners.mutations;
            state->activeScene = owners.activeScene;
            state->activeScenePath = owners.activeScenePath;
            state->triggerBinding = owners.triggerBinding;
            state->inventory = owners.inventory;
            state->activation = std::move(activation);
            state->saved = std::move(loaded).Value();
            state->draft = state->saved.settings;
            state->history.push_back(state->draft);
            state->store =
                std::make_unique<ProjectCollisionSettingsStore>(owners.files, owners.mutations, root, state->projectId, owners.source);
            if (owners.activeScene) {
                auto scene = owners.activeScene->Snapshot();
                if (scene.HasError())
                    return Return::Failure(scene.ErrorValue());
                state->sceneOpening = scene.Value().session;
            }
            // The private constructor requires direct construction; unique ownership protects control-block admission.
            auto owned = std::unique_ptr<CollisionProjectSettingsSession>{new CollisionProjectSettingsSession{std::move(state)}};
            return Return::Success(std::move(owned));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Capacity)));
        }
    }

    /** @copydoc CollisionProjectSettingsSession::CollisionProjectSettingsSession */
    CollisionProjectSettingsSession::CollisionProjectSettingsSession(std::unique_ptr<State> state) : state_(std::move(state)) {}

    /** @copydoc CollisionProjectSettingsSession::~CollisionProjectSettingsSession */
    CollisionProjectSettingsSession::~CollisionProjectSettingsSession() {
        Close();
    }

    /** @copydoc CollisionProjectSettingsSession::Snapshot */
    Result<std::shared_ptr<const Application::ProjectCollisionSettings>> CollisionProjectSettingsSession::Snapshot() const {
        if (const auto admitted = state_->Admit(); admitted.HasError())
            return Result<std::shared_ptr<const Application::ProjectCollisionSettings>>::Failure(admitted.ErrorValue());
        return Result<std::shared_ptr<const Application::ProjectCollisionSettings>>::Success(state_->draft);
    }

    /** @copydoc CollisionProjectSettingsSession::State::NewIdentity */
    template <typename Id> Result<Id> CollisionProjectSettingsSession::State::NewIdentity() const {
        if (const auto admitted = Admit(); admitted.HasError())
            return Result<Id>::Failure(admitted.ErrorValue());
        auto bytes = AllocateCollisionIdentityBytes(draft->Schema().get());
        if (bytes.HasError())
            return Result<Id>::Failure(bytes.ErrorValue());
        return Result<Id>::Success(Id::FromBytes(bytes.Value()));
    }

    /** @copydoc CollisionProjectSettingsSession::NewLayerId */
    Result<Physics::CollisionLayerId> CollisionProjectSettingsSession::NewLayerId() const {
        return state_->NewIdentity<Physics::CollisionLayerId>();
    }

    /** @copydoc CollisionProjectSettingsSession::NewProfileId */
    Result<Physics::CollisionProfileId> CollisionProjectSettingsSession::NewProfileId() const {
        return state_->NewIdentity<Physics::CollisionProfileId>();
    }

    /** @copydoc CollisionProjectSettingsSession::NewChannelId */
    Result<Physics::PhysicsQueryChannelId> CollisionProjectSettingsSession::NewChannelId() const {
        return state_->NewIdentity<Physics::PhysicsQueryChannelId>();
    }

    /** @copydoc CollisionProjectSettingsSession::ObserveActivation */
    Result<void> CollisionProjectSettingsSession::ObserveActivation(CollisionActivationEvidence evidence) {
        if (const auto admitted = state_->Admit(); admitted.HasError())
            return admitted;
        if (const bool exhaustedRevocation = evidence.revision == UINT64_MAX && state_->activation.revision == UINT64_MAX &&
                                             evidence.state == CollisionActivationState::Unknown;
            !ValidActivation(evidence, state_->projectId) || (evidence.revision <= state_->activation.revision && !exhaustedRevocation))
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        state_->activation = std::move(evidence);
        return Result<void>::Success();
    }

    /** @copydoc CollisionProjectSettingsSession::ObserveInventory */
    Result<void> CollisionProjectSettingsSession::ObserveInventory(const CollisionInventoryEvidence evidence) {
        if (const auto admitted = state_->Admit(); admitted.HasError())
            return admitted;
        if (static_cast<unsigned>(evidence.status) > static_cast<unsigned>(Assets::AssetRegistryBuildStatus::Failed))
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Invalid)));
        if (evidence.status == Assets::AssetRegistryBuildStatus::Complete &&
            (evidence.revision.value == 0 || evidence.revision != state_->authorities.registry->Snapshot().Revision()))
            return Result<void>::Failure(MakeError(CollisionSettingsError(CollisionSettingsFailure::Stale)));
        state_->inventory = evidence;
        return Result<void>::Success();
    }

    /** @copydoc CollisionProjectSettingsSession::Activation */
    Result<const CollisionActivationEvidence *> CollisionProjectSettingsSession::Activation() const {
        if (const auto admitted = state_->Admit(); admitted.HasError())
            return Result<const CollisionActivationEvidence *>::Failure(admitted.ErrorValue());
        return Result<const CollisionActivationEvidence *>::Success(&state_->activation);
    }

    /** @copydoc CollisionProjectSettingsSession::IsDirty */
    bool CollisionProjectSettingsSession::IsDirty() const noexcept {
        return state_->owner == std::this_thread::get_id() && !state_->closed && state_->dirty;
    }

    /** @copydoc CollisionProjectSettingsSession::Close */
    void CollisionProjectSettingsSession::Close() noexcept {
        if (!state_ || state_->owner != std::this_thread::get_id() || state_->closed)
            return;
        state_->closed = true;
        state_->opening.reset();
        state_->history.clear();
        state_->draft.reset();
        state_->saved.settings.reset();
        state_->activeScene = nullptr;
    }
}  // namespace Horo::Editor
