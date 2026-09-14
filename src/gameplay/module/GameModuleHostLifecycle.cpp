#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameplayErrors.h"

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

    LoadedGameModule::Impl::~Impl() {
        Shutdown();
    }

    Result<void> LoadedGameModule::Impl::RegisterAndStart(const std::span<const GameplayCapabilityId> hostCapabilities) {
        components = std::make_unique<ComponentRegistry>();
        assetTypes = std::make_unique<GameAssetTypeRegistry>(moduleId);
        services = std::make_unique<GameServiceRegistry>(moduleId);
        systems = std::make_unique<SystemRegistry>(moduleId);
        replication = std::make_unique<ReplicationRegistrationRegistry>(moduleId);
        GameRegistrationContext registration{moduleId, *components, *systems, *services, *assetTypes, *replication};
        if (Result<void> registered = InvokeRegister(*gameplayModule, registration); registered.HasError())
            return registered;
        if (Result<void> frozen = components->Freeze(); frozen.HasError())
            return frozen;
        if (Result<void> frozen = assetTypes->Freeze(); frozen.HasError())
            return frozen;
        if (Result<void> frozen = services->Freeze(hostCapabilities); frozen.HasError())
            return frozen;
        const std::vector<GameplayServiceId> serviceIds = ServiceIds(*services);
        const std::vector<GameplayCapabilityId> capabilities = CombinedCapabilities(hostCapabilities, *services);
        if (Result<void> frozen = systems->Freeze(serviceIds, capabilities); frozen.HasError())
            return frozen;
        if (Result<void> frozen = replication->Freeze(components->Descriptors(), registry->Registrations(), services->Registrations());
            frozen.HasError())
            return frozen;
        Detail::GenerationLeaseBinding::Bind(*registry, *systems, weak_from_this(), runtimeLeaseAdmission);
        Detail::GenerationLeaseBinding::Bind(*replication, weak_from_this(), runtimeLeaseAdmission);

        auto activated = GameplayServiceRuntime::Create(*services, GameplayServiceScope::Project, {{}, hostCapabilities});
        if (activated.HasError())
            return Result<void>::Failure(activated.ErrorValue());
        projectServices = std::move(activated).Value();
        runtimeContext = {projectServices->Cancellation(), projectServices->ActiveServices(), projectServices->Capabilities()};
        startAttempted = true;
        return InvokeStart(*gameplayModule, runtimeContext);
    }

    Result<GameModuleReloadSnapshot> LoadedGameModule::Impl::PrepareReload() {
        if (reloadPrepared || gameplayModule == nullptr || projectServices == nullptr)
            return Result<GameModuleReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired));
        projectServices->RequestCancellation();
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
        if (projectServices)
            projectServices->RequestCancellation();
        if (gameplayModule != nullptr && startAttempted)
            gameplayModule->Stop(runtimeContext);  // NOSONAR: exact-generation module boundary; path analysis is unrelated.
        projectServices.reset();
        assetTypes.reset();
        replication.reset();
        if (gameplayModule != nullptr) {
            destroy(gameplayModule);
            gameplayModule = nullptr;
        }
        registry.reset();
        systems.reset();
        services.reset();
        components.reset();
        library.reset();
        if (removeArtifactOnUnload) {
            std::error_code ignored;
            std::filesystem::remove(loadedArtifactPath, ignored);  // NOSONAR: canonical host-created shadow artifact only.
        }
    }
}  // namespace Horo::Gameplay
