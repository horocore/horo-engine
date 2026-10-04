#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <utility>

namespace Horo::Gameplay {
    LoadedGameModule::LoadedGameModule(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    LoadedGameModule::~LoadedGameModule() = default;

    /** @copydoc LoadedGameModule::ModuleId */
    const std::string &LoadedGameModule::ModuleId() const noexcept {
        return impl_->moduleId;
    }

    /** @copydoc LoadedGameModule::BuildFingerprint */
    const std::string &LoadedGameModule::BuildFingerprint() const noexcept {
        return impl_->buildFingerprint;
    }

    /** @copydoc LoadedGameModule::DescriptorRevision */
    std::uint64_t LoadedGameModule::DescriptorRevision() const noexcept {
        return impl_->descriptorRevision;
    }

    /** @copydoc LoadedGameModule::LoadedArtifactPath */
    const std::filesystem::path &LoadedGameModule::LoadedArtifactPath() const noexcept {
        return impl_->loadedArtifactPath;
    }

    /** @copydoc LoadedGameModule::Registry */
    const BehaviorRegistry &LoadedGameModule::Registry() const noexcept {
        return *impl_->registries.registry;
    }

    /** @copydoc LoadedGameModule::ContributeBehaviorsTo */
    Result<void> LoadedGameModule::ContributeBehaviorsTo(BehaviorRegistry &destination) const {
        for (const BehaviorRegistration &registration : impl_->registries.registry->Registrations()) {
            if (Result<void> contributed = destination.Register(registration); contributed.HasError())
                return contributed;
        }
        Detail::GenerationLeaseBinding::Bind(destination, std::weak_ptr<void>{impl_}, impl_->runtimeLeaseAdmission);
        return Result<void>::Success();
    }

    /** @copydoc LoadedGameModule::Components */
    const ComponentRegistry &LoadedGameModule::Components() const noexcept {
        return *impl_->registries.components;
    }

    /** @copydoc LoadedGameModule::AssetTypes */
    const GameAssetTypeRegistry &LoadedGameModule::AssetTypes() const noexcept {
        return *impl_->registries.assetTypes;
    }

    /** @copydoc LoadedGameModule::Services */
    const GameServiceRegistry &LoadedGameModule::Services() const noexcept {
        return *impl_->registries.services;
    }

    /** @copydoc LoadedGameModule::Systems */
    const SystemRegistry &LoadedGameModule::Systems() const noexcept {
        return *impl_->registries.systems;
    }

    /** @copydoc LoadedGameModule::Replication */
    const ReplicationRegistrationRegistry &LoadedGameModule::Replication() const noexcept {
        return *impl_->registries.replication;
    }

    /** @copydoc LoadedGameModule::Events */
    const GameEventRegistry &LoadedGameModule::Events() const noexcept {
        return *impl_->registries.events;
    }

    /** @copydoc LoadedGameModule::AcquirePersistence */
    Result<std::shared_ptr<Runtime::GameplayPersistenceAdapter>> LoadedGameModule::AcquirePersistence(
        const Runtime::SaveParticipantId &participant) const {
        if (!impl_->runtimeLeaseAdmission.load(std::memory_order_acquire))
            return Result<std::shared_ptr<Runtime::GameplayPersistenceAdapter>>::Failure(
                MakeError(GameplayErrors::GameplayReloadRestartRequired));
        for (const auto &registration : impl_->registries.persistence->registrations_) {
            if (registration.descriptor.participant.participant == participant)
                return Runtime::GameplayPersistenceAdapter::Create(registration.descriptor, registration.source, impl_);
        }
        return Result<std::shared_ptr<Runtime::GameplayPersistenceAdapter>>::Failure(
            MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
    }

    /** @copydoc LoadedGameModule::ActiveServices */
    std::span<const GameplayServiceId> LoadedGameModule::ActiveServices() const noexcept {
        return impl_->runtimeContext.activeServices;
    }

    /** @copydoc LoadedGameModule::Capabilities */
    std::span<const GameplayCapabilityId> LoadedGameModule::Capabilities() const noexcept {
        return impl_->runtimeContext.capabilities;
    }

    /** @copydoc LoadedGameModule::Cancellation */
    CancellationToken LoadedGameModule::Cancellation() const noexcept {
        return impl_->runtimeContext.cancellation;
    }

    /** @copydoc LoadedGameModule::PhysicsContext */
    std::shared_ptr<const GameplayPhysicsContext> LoadedGameModule::PhysicsContext() const noexcept {
        return impl_->physics;
    }

    /** @copydoc LoadedGameModule::PrepareReload */
    Result<GameModuleReloadSnapshot> LoadedGameModule::PrepareReload() {  // NOSONAR(cpp:S5817) Mutates generation lifecycle.
        impl_->runtimeLeaseAdmission.store(false, std::memory_order_release);
        if (impl_->physics)
            impl_->physics->Revoke();
        if (impl_.use_count() != 1)
            return Result<GameModuleReloadSnapshot>::Failure(
                MakeError(GameplayErrors::GameplayReloadRestartRequired, "A module-generation runtime is still active."));
        return impl_->PrepareReload();
    }

    /** @copydoc LoadedGameModule::RestoreReload */
    Result<void> LoadedGameModule::RestoreReload(  // NOSONAR(cpp:S5817) Mutates generation lifecycle.
        const GameModuleReloadSnapshot &snapshot) {
        return impl_->RestoreReload(snapshot);
    }

    /** @copydoc GameModuleHost::GameModuleHost */
    GameModuleHost::GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities) : hostCapabilities_(std::move(hostCapabilities)) {}

    /** @copydoc GameModuleHost::GameModuleHost */
    GameModuleHost::GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities, std::shared_ptr<GameplayPhysicsContext> physics)
        : hostCapabilities_(std::move(hostCapabilities)), physics_(std::move(physics)) {}
}  // namespace Horo::Gameplay
