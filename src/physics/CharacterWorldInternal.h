#pragma once

#include "CharacterControllerRegistry.h"
#include "CharacterFastPathStorage.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Character {
    namespace Detail {
        /** @brief Target-private owned controller state prepared for later fixed-tick behavior. */
        struct CharacterControllerRecord final {
            CharacterControllerDescriptor descriptor;
            std::optional<CharacterMovementRequest> lastMovement;
            CharacterTransformPublication publication;
            std::uint64_t lastSequence{};
            std::uint64_t lastTeleportTick{};
            std::optional<std::uint64_t> reservedTeleportTick;
            bool spawned{};
        };

        /** @brief Process-owned synchronization and non-wrapping Character world identity source. */
        struct CharacterWorldIdentityAuthority final {
            std::mutex mutex;
            std::uint64_t next{1};
        };

        /** @brief Encapsulates command and publication mutex ownership for one Character world. */
        class CharacterWorldSynchronization final {
        public:
            [[nodiscard]] std::unique_lock<std::mutex> TryLockCommands() {
                return std::unique_lock{commandMutex_, std::try_to_lock};
            }

            [[nodiscard]] std::scoped_lock<std::mutex> LockCommands() const {
                return std::scoped_lock{commandMutex_};
            }

            [[nodiscard]] std::scoped_lock<std::mutex> LockPublication() const {
                return std::scoped_lock{publicationMutex_};
            }

            [[nodiscard]] std::scoped_lock<std::mutex> LockRegistry() const {
                return std::scoped_lock{registryMutex_};
            }

        private:
            mutable std::mutex commandMutex_;
            mutable std::mutex publicationMutex_;
            mutable std::mutex registryMutex_;
        };

        [[nodiscard]] inline CharacterWorldIdentityAuthority &WorldIdentityAuthority() {
            static CharacterWorldIdentityAuthority authority;
            return authority;
        }

        [[nodiscard]] inline Result<CharacterWorldDescriptor> CompleteWorldDescriptor(
            const CharacterWorldPreparationDescriptor &descriptor) {
            if (const std::array valid{descriptor.sceneGeneration != 0, descriptor.physicsWorld.IsValid(),
                                       descriptor.collisionFilterGeneration != 0, descriptor.originGeneration != 0,
                                       descriptor.physicsSnapshotRevision != 0};
                !std::ranges::all_of(valid, std::identity{})) {
                return Result<CharacterWorldDescriptor>::Failure(MakeError(CharacterErrors::WorldInvalid));
            }
            CharacterWorldIdentityAuthority &authority = WorldIdentityAuthority();
            const std::lock_guard identityLock{authority.mutex};
            if (authority.next == std::numeric_limits<std::uint64_t>::max())
                return Result<CharacterWorldDescriptor>::Failure(MakeError(CharacterErrors::GenerationExhausted));
            const std::uint64_t identityValue = authority.next++;
            const auto identity = CharacterWorldId::Create(identityValue);
            if (identity.HasError())
                return Result<CharacterWorldDescriptor>::Failure(identity.ErrorValue());
            return Result<CharacterWorldDescriptor>::Success({descriptor.sceneGeneration, identity.Value(), descriptor.physicsWorld,
                                                              descriptor.collisionFilterGeneration, descriptor.originGeneration,
                                                              descriptor.physicsSnapshotRevision});
        }

        [[nodiscard]] inline Result<void> RequireOwnerThread(const std::thread::id ownerThread) {
            if (ownerThread != std::this_thread::get_id())
                return Result<void>::Failure(
                    MakeError(CharacterErrors::InvalidState, "Character world mutation requires its preparation thread."));
            return Result<void>::Success();
        }

        [[nodiscard]] inline Result<void> RequirePlacementMutation(const auto &impl) {
            if (const auto owner = RequireOwnerThread(impl.ownerThread); owner.HasError())
                return owner;
            if (impl.state.load() == CharacterWorldState::Destroyed || impl.ticking.load() || impl.placementActive)
                return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
            return Result<void>::Success();
        }
    }  // namespace Detail

    struct CharacterWorld::Impl final {  // NOSONAR(cpp:S1820) -- the flat hot-state layout keeps command and lifecycle data contiguous.

        Impl(const CharacterWorldDescriptor &owner, const CharacterWorldSettings &worldSettings,
             Detail::CharacterControllerRegistry<Detail::CharacterControllerRecord> &&controllerRegistry)
            : descriptor(owner), settings(worldSettings), controllers(std::move(controllerRegistry)), fastPath(settings),
              controllerGenerations(settings.Values().capacities.maximumControllers),
              closedSequences(settings.Values().capacities.maximumControllers) {}

        CharacterWorldDescriptor descriptor;
        CharacterWorldSettings settings;
        Detail::CharacterControllerRegistry<Detail::CharacterControllerRecord> controllers;
        Detail::CharacterFastPathStorage fastPath;
        std::vector<std::uint32_t> controllerGenerations;
        std::vector<std::uint64_t> closedSequences;
        Detail::CharacterWorldSynchronization synchronization;
        CharacterPublishedTick published;
        std::atomic<std::uint64_t> closedTick{};
        std::atomic<bool> acceptingCommands{};
        std::atomic<std::uint64_t> admittedCommands{};
        std::atomic<std::uint64_t> rejectedCommands{};
        std::atomic<std::uint64_t> commandOverflowCount{};
        std::atomic<std::uint64_t> completedTicks{};
        std::atomic<std::uint32_t> pendingCommands{};
        std::atomic<std::uint32_t> maximumCommandDepth{};
        std::thread::id ownerThread{std::this_thread::get_id()};
        std::atomic<CharacterWorldState> state{CharacterWorldState::Prepared};
        std::atomic<bool> ticking{};
        bool placementActive{};
        bool shutdownRequested{};
    };

    namespace Detail {
        /** @brief Drains controller storage after an owner-thread shutdown deferred by a guarded operation. */
        template <typename Impl> void DrainDeferredShutdown(Impl &impl) noexcept {
            if (!impl.shutdownRequested)
                return;
            const auto registryLock = impl.synchronization.LockRegistry();
            impl.controllers.Drain();
            impl.shutdownRequested = false;
        }
    }  // namespace Detail
}  // namespace Horo::Character
