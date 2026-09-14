#pragma once

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Gameplay/SystemRegistry.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace Horo::Gameplay {
    namespace Detail {
        struct GenerationLeaseBinding {
            static void Bind(BehaviorRegistry &behaviors, const std::weak_ptr<void> &lease, const std::atomic_bool &admission) noexcept {
                behaviors.generationLease_ = lease;
                behaviors.generationLeaseAdmission_ = &admission;
            }

            static void Bind(SystemRegistry &systems, const std::weak_ptr<void> &lease, const std::atomic_bool &admission) noexcept {
                systems.generationLease_ = lease;
                systems.generationLeaseAdmission_ = &admission;
            }

            static void Bind(BehaviorRegistry &behaviors, SystemRegistry &systems, const std::weak_ptr<void> &lease,
                             const std::atomic_bool &admission) noexcept {
                Bind(behaviors, lease, admission);
                Bind(systems, lease, admission);
            }

            static void Bind(ReplicationRegistrationRegistry &replication, const std::weak_ptr<void> &lease,
                             const std::atomic_bool &admission) noexcept {
                replication.generationLease_ = lease;
                replication.generationLeaseAdmission_ = &admission;
            }
        };
    }  // namespace Detail

    struct LoadedGameModule::Impl : std::enable_shared_from_this<LoadedGameModule::Impl> {
        ~Impl();

        [[nodiscard]] Result<void> RegisterAndStart(std::span<const GameplayCapabilityId> hostCapabilities);
        [[nodiscard]] Result<GameModuleReloadSnapshot> PrepareReload();
        [[nodiscard]] Result<void> RestoreReload(const GameModuleReloadSnapshot &snapshot);
        void Shutdown() noexcept;

        std::unique_ptr<Platform::DynamicLibrary> library;
        std::unique_ptr<BehaviorRegistry> registry;
        std::unique_ptr<ComponentRegistry> components;
        std::unique_ptr<GameAssetTypeRegistry> assetTypes;
        std::unique_ptr<GameServiceRegistry> services;
        std::unique_ptr<SystemRegistry> systems;
        std::unique_ptr<ReplicationRegistrationRegistry> replication;
        std::unique_ptr<GameplayServiceRuntime> projectServices;
        GameRuntimeContext runtimeContext;
        IGameModule *gameplayModule{};
        DestroyGameModuleFunction destroy{};
        std::string moduleId;
        std::string buildFingerprint;
        std::uint64_t descriptorRevision{};
        std::filesystem::path loadedArtifactPath;
        std::atomic_bool runtimeLeaseAdmission{true};
        bool removeArtifactOnUnload{};
        bool startAttempted{};
        bool reloadPrepared{};
        bool shutdown{};
    };
}  // namespace Horo::Gameplay
