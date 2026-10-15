#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Ui/RuntimeUiClient.h"

#include <utility>

namespace Horo::Gameplay {
    LoadedGameModule::LoadedGameModule(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    LoadedGameModule::~LoadedGameModule() {
        if (impl_ && impl_->runtimeUi)
            impl_->runtimeUi->Revoke();
        if (impl_ && impl_->character)
            impl_->character->Revoke();
        if (impl_ && impl_->navigation)
            impl_->navigation->Revoke();
    }

    /** @copydoc LoadedGameModule::NavigationContext */
    std::shared_ptr<const GameplayNavigationContext> LoadedGameModule::NavigationContext() const noexcept {
        return impl_->navigation;
    }

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

    /** @copydoc LoadedGameModule::AcquireInstalledPersistence */
    Result<Runtime::GameplayPersistenceInstallation> LoadedGameModule::AcquireInstalledPersistence(
        const Runtime::SaveParticipantId &participant) const {
        auto adapter = AcquirePersistence(participant);
        if (adapter.HasError())
            return Result<Runtime::GameplayPersistenceInstallation>::Failure(adapter.ErrorValue());
        std::shared_ptr<const std::atomic_bool> admission{impl_, &impl_->runtimeLeaseAdmission};
        auto ownedAdapter = std::move(adapter).Value();
        std::shared_ptr<const Runtime::GameplayPersistenceDescriptor> descriptor{ownedAdapter, &ownedAdapter->Descriptor()};
        Runtime::GameplayPersistenceInstallation receipt{std::move(descriptor), std::move(ownedAdapter), std::move(admission),
                                                         Cancellation()};
        if (!receipt.CanUse())
            return Result<Runtime::GameplayPersistenceInstallation>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired));
        return Result<Runtime::GameplayPersistenceInstallation>::Success(std::move(receipt));
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

    /** @copydoc LoadedGameModule::RuntimeUiContext */
    std::shared_ptr<const Runtime::Ui::RuntimeUiClient> LoadedGameModule::RuntimeUiContext() const noexcept {
        return impl_->runtimeUi;
    }

    /** @copydoc LoadedGameModule::CharacterContext */
    std::shared_ptr<const GameplayCharacterContext> LoadedGameModule::CharacterContext() const noexcept {
        return impl_->character;
    }

    /** @copydoc LoadedGameModule::PrepareReload */
    Result<GameModuleReloadSnapshot> LoadedGameModule::PrepareReload() {  // NOSONAR(cpp:S5817) Mutates generation lifecycle.
        impl_->runtimeLeaseAdmission.store(false, std::memory_order_release);
        if (impl_->runtimeUi)
            impl_->runtimeUi->Revoke();
        if (impl_ && impl_->character)
            impl_->character->Revoke();
        if (impl_ && impl_->navigation)
            impl_->navigation->Revoke();
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
    GameModuleHost::GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities)
        : GameModuleHost(GameModuleHostOptions{std::move(hostCapabilities), {}}) {}

    /** @copydoc GameModuleHost::GameModuleHost */
    GameModuleHost::GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities, std::shared_ptr<GameplayPhysicsContext> physics,
                                   std::shared_ptr<GameplayNavigationContext> navigation)
        : GameModuleHost(
              GameModuleHostOptions{std::move(hostCapabilities), {.physics = std::move(physics), .navigation = std::move(navigation)}}) {}

    /** @copydoc GameModuleHost::GameModuleHost */
    GameModuleHost::GameModuleHost(std::vector<GameplayCapabilityId> hostCapabilities, std::shared_ptr<GameplayPhysicsContext> physics)
        : GameModuleHost(GameModuleHostOptions{std::move(hostCapabilities), {.physics = std::move(physics)}}) {}

    /** @copydoc GameModuleHost::GameModuleHost */
    GameModuleHost::GameModuleHost(GameModuleHostOptions options)
        : hostCapabilities_(std::move(options.hostCapabilities)), runtimeServices_(std::move(options.runtimeServices)) {}
}  // namespace Horo::Gameplay
