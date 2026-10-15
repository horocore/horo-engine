#pragma once

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameEventRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/GameplayAiContext.h"
#include "Horo/Gameplay/GameplayRegistrationRuntime.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Gameplay/SystemRegistry.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace Horo::Gameplay {
    namespace Detail {
        /** @brief Pin native SDK deleter code independently of module instances and defer owned shadow cleanup. */
        struct GameModuleCodeImage final {
            explicit GameModuleCodeImage(std::unique_ptr<Platform::DynamicLibrary> loaded) : library(std::move(loaded)) {}

            /** @brief Unload mapped SDK code before removing its exact host-created shadow artifact. */
            ~GameModuleCodeImage() noexcept;
            std::unique_ptr<Platform::DynamicLibrary> library;
            std::filesystem::path shadowArtifact;
        };

        /** @brief Generation-owned native contributions; shutdown retires each registry before the code image. */
        struct NativeRegistries {
            std::unique_ptr<BehaviorRegistry> registry;
            AI::AiCapabilityDescriptorRegistry ai;
            std::unique_ptr<ComponentRegistry> components;
            std::unique_ptr<GameAssetTypeRegistry> assetTypes;
            std::unique_ptr<GameServiceRegistry> services;
            std::unique_ptr<SystemRegistry> systems;
            std::unique_ptr<ReplicationRegistrationRegistry> replication;
            std::unique_ptr<GameEventRegistry> events;
            std::unique_ptr<PersistenceRegistrationRegistry> persistence;
        };

        struct GenerationLeaseBinding {
            static void Bind(GameEventRegistry &events, const std::weak_ptr<void> &lease, const std::atomic_bool &admission) noexcept {
                events.generationLease_ = lease;
                events.generationLeaseAdmission_ = &admission;
            }

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

        /** @brief Freezes contributed metadata and binds exact-generation leases before any runtime activation. */
        [[nodiscard]] Result<void> FreezeRegistries(std::span<const GameplayCapabilityId> hostCapabilities);
        [[nodiscard]] Result<void> RegisterAndStart(std::span<const GameplayCapabilityId> hostCapabilities);
        [[nodiscard]] Result<GameModuleReloadSnapshot> PrepareReload();
        [[nodiscard]] Result<void> RestoreReload(const GameModuleReloadSnapshot &snapshot);
        /** @brief Expose the already admitted scoped holders only after project service activation succeeds. */
        void PublishRuntimeContext();
        void Shutdown() noexcept;

        std::shared_ptr<Detail::GameModuleCodeImage> codeImage;
        Detail::NativeRegistries registries;
        std::unique_ptr<GameplayServiceRuntime> projectServices;
        GameRuntimeContext runtimeContext;
        std::shared_ptr<GameplayPhysicsContext> physics;
        std::shared_ptr<GameplayAiContext> ai;
        std::shared_ptr<GameplayCharacterContext> character;
        std::shared_ptr<GameplayNavigationContext> navigation;
        IGameModule *gameplayModule{};
        DestroyGameModuleFunction destroy{};
        std::string moduleId;
        std::string buildFingerprint;
        std::uint64_t descriptorRevision{};
        std::filesystem::path loadedArtifactPath;
        std::atomic_bool runtimeLeaseAdmission{true};
        std::thread::id ownerThread{std::this_thread::get_id()};
        bool startAttempted{};
        bool reloadPrepared{};
        bool shutdown{};
    };
}  // namespace Horo::Gameplay
