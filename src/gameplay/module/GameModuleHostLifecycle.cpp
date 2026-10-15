#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <algorithm>
#include <system_error>

namespace Horo::Gameplay {
    namespace {
        [[nodiscard]] std::vector<GameplayServiceId> ServiceIds(const GameServiceRegistry &registry) {
            std::vector<GameplayServiceId> ids;
            ids.reserve(registry.Registrations().size());
            for (const GameplayServiceRegistration &registration : registry.Registrations())
                ids.push_back(registration.descriptor.id);
            return ids;
        }

        [[nodiscard]] std::vector<GameplayCapabilityId> CombinedCapabilities(const std::span<const GameplayCapabilityId> hostCapabilities,
                                                                             const GameServiceRegistry &registry) {
            std::vector<GameplayCapabilityId> capabilities(hostCapabilities.begin(), hostCapabilities.end());
            capabilities.insert(capabilities.end(), registry.ProvidedCapabilities().begin(), registry.ProvidedCapabilities().end());
            std::ranges::sort(capabilities);
            return capabilities;
        }

        [[nodiscard]] Result<void> InvokeRegister(IGameModule &gameModule, GameRegistrationContext &context) noexcept {
            try {
                return gameModule.Register(context);
            } catch (...) {
                return Result<void>::Failure(
                    MakeError(GameplayErrors::GameplayFactoryFailed, "Gameplay module registration threw an exception."));
            }
        }

        [[nodiscard]] Result<void> InvokeStart(IGameModule &gameModule, GameRuntimeContext &context) noexcept {
            try {
                return gameModule.Start(context);
            } catch (...) {
                return Result<void>::Failure(
                    MakeError(GameplayErrors::GameplayFactoryFailed, "Gameplay module startup threw an exception."));
            }
        }

        [[nodiscard]] Result<GameModuleReloadSnapshot> InvokePrepareReload(IGameModule &gameModule, GameRuntimeContext &context) noexcept {
            try {
                return gameModule.PrepareReload(context);
            } catch (...) {
                return Result<GameModuleReloadSnapshot>::Failure(
                    MakeError(GameplayErrors::GameplayReloadRestartRequired, "Gameplay reload quiescence callback threw an exception."));
            }
        }

        [[nodiscard]] Result<void> InvokeRestoreReload(IGameModule &gameModule, const GameModuleReloadSnapshot &snapshot,
                                                       GameRuntimeContext &context) noexcept {
            try {
                return gameModule.RestoreReload(snapshot, context);
            } catch (...) {
                return Result<void>::Failure(
                    MakeError(GameplayErrors::GameplayReloadRestoreFailed, "Gameplay reload restore callback threw an exception."));
            }
        }
    }  // namespace

    /** @copydoc Detail::GameModuleCodeImage::~GameModuleCodeImage */
    Detail::GameModuleCodeImage::~GameModuleCodeImage() noexcept {
        library.reset();
        if (!shadowArtifact.empty()) {
            std::error_code ignored;
            std::filesystem::remove(shadowArtifact, ignored);  // NOSONAR: canonical host-created shadow artifact only.
        }
    }

    LoadedGameModule::Impl::~Impl() {
        Shutdown();
    }

    /** @copydoc LoadedGameModule::Impl::FreezeRegistries */
    Result<void> LoadedGameModule::Impl::FreezeRegistries(const std::span<const GameplayCapabilityId> hostCapabilities) {
        if (Result<void> frozen = registries.components->Freeze(); frozen.HasError())
            return frozen;
        if (Result<void> frozen = registries.assetTypes->Freeze(); frozen.HasError())
            return frozen;
        if (Result<void> frozen = registries.services->Freeze(hostCapabilities); frozen.HasError())
            return frozen;
        const std::vector<GameplayServiceId> serviceIds = ServiceIds(*registries.services);
        const std::vector<GameplayCapabilityId> capabilities = CombinedCapabilities(hostCapabilities, *registries.services);
        if (Result<void> frozen = registries.systems->Freeze(serviceIds, capabilities); frozen.HasError())
            return frozen;
        if (Result<void> frozen = registries.replication->Freeze(registries.components->Descriptors(), registries.registry->Registrations(),
                                                                 registries.services->Registrations());
            frozen.HasError())
            return frozen;
        Detail::GenerationLeaseBinding::Bind(*registries.registry, *registries.systems, weak_from_this(), runtimeLeaseAdmission);
        Detail::GenerationLeaseBinding::Bind(*registries.replication, weak_from_this(), runtimeLeaseAdmission);
        if (Result<void> frozen =
                registries.persistence->Freeze(registries.registry->Registrations(), registries.services->Registrations());
            frozen.HasError())
            return frozen;
        const auto aiDeclarations = registries.ai.Freeze();
        if (ai)
            ai->declarations_ = aiDeclarations;
        registries.events->Freeze();
        Detail::GenerationLeaseBinding::Bind(*registries.events, weak_from_this(), runtimeLeaseAdmission);

        return Result<void>::Success();
    }

    Result<void> LoadedGameModule::Impl::RegisterAndStart(const std::span<const GameplayCapabilityId> hostCapabilities) {
        registries.components = std::make_unique<ComponentRegistry>();
        registries.assetTypes = std::make_unique<GameAssetTypeRegistry>(moduleId);
        registries.services = std::make_unique<GameServiceRegistry>(moduleId);
        registries.systems = std::make_unique<SystemRegistry>(moduleId);
        registries.replication = std::make_unique<ReplicationRegistrationRegistry>(moduleId);
        registries.events = std::make_unique<GameEventRegistry>();
        registries.persistence = std::make_unique<PersistenceRegistrationRegistry>(moduleId);
        GameRegistrationContext registration{moduleId,
                                             *registries.components,
                                             *registries.systems,
                                             *registries.services,
                                             *registries.assetTypes,
                                             *registries.replication,
                                             *registries.events,
                                             *registries.persistence,
                                             registries.ai};
        if (Result<void> registered = InvokeRegister(*gameplayModule, registration); registered.HasError())
            return registered;
        if (auto frozen = FreezeRegistries(hostCapabilities); frozen.HasError())
            return frozen;

        auto activated = GameplayServiceRuntime::Create(*registries.services, GameplayServiceScope::Project, {{}, hostCapabilities});
        if (activated.HasError())
            return Result<void>::Failure(activated.ErrorValue());
        projectServices = std::move(activated).Value();
        PublishRuntimeContext();
        startAttempted = true;
        return InvokeStart(*gameplayModule, runtimeContext);
    }

    /** @copydoc LoadedGameModule::Impl::PublishRuntimeContext */
    void LoadedGameModule::Impl::PublishRuntimeContext() {
        runtimeContext = {projectServices->Cancellation(), projectServices->ActiveServices(), projectServices->Capabilities()};
        runtimeContext.physics = physics;
        runtimeContext.character = character;
        runtimeContext.ai = ai;
        runtimeContext.navigation = navigation;
    }

    Result<GameModuleReloadSnapshot> LoadedGameModule::Impl::PrepareReload() {
        if (reloadPrepared || gameplayModule == nullptr || projectServices == nullptr)
            return Result<GameModuleReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired));
        projectServices->RequestCancellation();
        if (ai) {
            ai->Revoke();
            ai->RetireOwnedTasksAtSafePoint();
        }
        if (character)
            character->Revoke();
        if (navigation)
            navigation->Revoke();
        if (physics)
            physics->Revoke();
        auto snapshot = InvokePrepareReload(*gameplayModule, runtimeContext);
        if (snapshot.HasError())
            return Result<GameModuleReloadSnapshot>::Failure(snapshot.ErrorValue());
        if (snapshot.Value().schemaVersion == 0 || snapshot.Value().payload.size() > MaximumGameModuleReloadStateBytes)
            return Result<GameModuleReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        if (startAttempted)
            gameplayModule->Stop(runtimeContext);
        projectServices.reset();
        runtimeContext.activeServices = {};
        runtimeContext.capabilities = {};
        runtimeContext.ai.reset();
        startAttempted = false;
        reloadPrepared = true;
        return snapshot;
    }

    Result<void> LoadedGameModule::Impl::RestoreReload(const GameModuleReloadSnapshot &snapshot) {
        if (reloadPrepared || gameplayModule == nullptr || projectServices == nullptr || snapshot.schemaVersion == 0 ||
            snapshot.payload.size() > MaximumGameModuleReloadStateBytes)
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadSnapshotInvalid));
        return InvokeRestoreReload(*gameplayModule, snapshot, runtimeContext);
    }

    void LoadedGameModule::Impl::Shutdown() noexcept {
        if (shutdown)
            return;
        shutdown = true;
        if (ai) {
            ai->Revoke();
            ai->RetireOwnedTasksAtSafePoint();
        }
        if (character)
            character->Revoke();
        if (navigation)
            navigation->Revoke();
        if (physics)
            physics->Revoke();
        if (projectServices)
            projectServices->RequestCancellation();
        if (gameplayModule != nullptr && startAttempted)
            gameplayModule->Stop(runtimeContext);  // NOSONAR: exact-generation module boundary; path analysis is unrelated.
        projectServices.reset();
        registries.assetTypes.reset();
        registries.replication.reset();
        registries.events.reset();
        registries.persistence.reset();
        if (gameplayModule != nullptr) {
            destroy(gameplayModule);
            runtimeContext.ai.reset();
            gameplayModule = nullptr;
        }
        runtimeContext.navigation.reset();
        navigation.reset();
        runtimeContext.character.reset();
        character.reset();
        registries.registry.reset();
        registries.systems.reset();
        registries.services.reset();
        registries.components.reset();
        codeImage.reset();
    }
}  // namespace Horo::Gameplay
