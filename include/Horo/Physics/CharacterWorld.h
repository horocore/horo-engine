#pragma once

/** @file CharacterWorld.h
 * @brief Per-scene Character controller ownership and bounded slot lifecycle.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Physics/CharacterCommandPipeline.h"
#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/CharacterWorldSettings.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace Horo::Character {
    /** @brief Immutable external generations required to prepare one detached Character-world candidate. */
    struct CharacterWorldPreparationDescriptor final {
        std::uint64_t sceneGeneration{};           /**< Exact scene generation that owns the world. */
        Physics::PhysicsWorldId physicsWorld;      /**< Exact paired Physics-world generation. */
        std::uint64_t collisionFilterGeneration{}; /**< Exact project collision-filter generation. */
        std::uint64_t originGeneration{};          /**< Exact local-origin generation. */
        std::uint64_t physicsSnapshotRevision{1};  /**< Exact Physics query snapshot revision. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldPreparationDescriptor &) const noexcept = default;
    };

    /** @brief Immutable complete owner generations retained by one prepared Character world. */
    struct CharacterWorldDescriptor final {
        std::uint64_t sceneGeneration{};           /**< Exact scene generation that owns the world. */
        CharacterWorldId identity;                 /**< Internally issued, never-reused process-local generation. */
        Physics::PhysicsWorldId physicsWorld;      /**< Exact paired Physics-world generation. */
        std::uint64_t collisionFilterGeneration{}; /**< Exact project collision-filter generation. */
        std::uint64_t originGeneration{};          /**< Exact local-origin generation. */
        std::uint64_t physicsSnapshotRevision{1};  /**< Exact Physics query snapshot revision. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterWorldDescriptor &) const noexcept = default;
    };

    /** @brief Observable lifecycle for a detached, published, or retired Character world. */
    enum class CharacterWorldState : std::uint8_t {
        Prepared,
        Active,
        Destroyed,
    };

    /**
     * @brief Owns bounded controller records for one exact scene and Physics-world generation.
     *
     * Preparation allocates the complete slot table before aggregate scene publication. Controller
     * descriptors may be installed while prepared so activation performs no allocation. Returned
     * handles are non-owning and remain valid only while their exact slot generation is resident.
     * The aggregate owner must shut this world down before its paired Physics world.
     */
    class CharacterWorld final {
    public:
        /**
         * @brief Creates an unpublished world candidate and reserves its complete controller capacity.
         * @param descriptor Exact external owner generations; the Character-world identity is issued internally.
         * @param settings Validated immutable settings snapshot copied into the candidate.
         * @return Prepared world, or a stable world/capacity error after complete rollback.
         * @post Success publishes no controller or scene state and retains no caller-owned storage.
         */
        [[nodiscard]] static Result<std::unique_ptr<CharacterWorld>> Prepare(const CharacterWorldPreparationDescriptor &descriptor,
                                                                             const CharacterWorldSettings &settings);

        /** @brief Drains controller records and retires the world if its aggregate owner omitted explicit shutdown. */
        ~CharacterWorld();
        CharacterWorld(const CharacterWorld &) = delete;
        CharacterWorld &operator=(const CharacterWorld &) = delete;

        /**
         * @brief Publishes a fully prepared candidate without allocating.
         * @return Success, or CharacterErrors::InvalidState unless the world is prepared.
         */
        [[nodiscard]] Result<void> Activate();

        /**
         * @brief Installs one validated owned controller descriptor into bounded world storage.
         * @param descriptor Inert descriptor bound to this world's exact owner generations.
         * @return Stable handle, or a typed descriptor/world/capacity/generation error.
         * @pre The world is prepared and the call occurs on its preparation thread.
         * @post Failure preserves every existing controller and slot generation.
         */
        [[nodiscard]] Result<CharacterControllerHandle> CreateController(const CharacterControllerDescriptor &descriptor);

        /**
         * @brief Spawns one structural controller through bounded overlap recovery.
         * @param handle Prepared controller handle.
         * @param query Read-only Physics overlap adapter for this operation.
         * @return One coherent publication, or a typed failure with no state mutation.
         * @pre The controller is not already spawned and the call runs on the world owner thread.
         */
        [[nodiscard]] Result<CharacterPlacementResult> SpawnController(const CharacterControllerHandle &handle,
                                                                       const CharacterPhysicsQueryContext &query);

        /**
         * @brief Removes one exact live controller generation and releases its owned record.
         * @param handle Handle issued by this world for a currently resident controller.
         * @return Success, or a typed malformed/foreign/stale/lifecycle error without mutation.
         * @pre The world is prepared and the call occurs on its preparation thread.
         */
        [[nodiscard]] Result<void> DestroyController(const CharacterControllerHandle &handle);

        /**
         * @brief Publishes an explicit teleport independently of movement resolution.
         * @param request Tick-addressed target root and heading.
         * @param query Current read-only Physics overlap snapshot for the target tick.
         * @return One coherent publication, or a typed failure that preserves the prior publication.
         * @pre The controller is spawned, the world is not resolving a tick, and the call runs on the owner thread.
         */
        [[nodiscard]] Result<CharacterPlacementResult> TeleportController(const CharacterTeleportRequest &request,
                                                                          const CharacterPhysicsQueryContext &query);

        /**
         * @brief Copies the inert descriptor for one exact live controller generation.
         * @param handle Handle issued by this world for a currently resident controller.
         * @return Owned descriptor copy, or a typed malformed/foreign/stale/lifecycle error.
         */
        [[nodiscard]] Result<CharacterControllerDescriptor> ControllerDescriptor(const CharacterControllerHandle &handle) const;

        /**
         * @brief Returns the last coherent spawn or teleport root publication.
         * @param handle Live spawned controller handle.
         * @return Owned snapshot or a typed lifecycle/handle error.
         */
        [[nodiscard]] Result<CharacterTransformPublication> ControllerTransform(const CharacterControllerHandle &handle) const;

        /**
         * @brief Copies one future tick-addressed movement request into bounded world storage without blocking.
         * @param request Immutable owned request; no live producer state is retained.
         * @return Deferred, full or busy admission, or a typed malformed/late/stale/lifecycle error.
         * @post Concurrent admission never mutates a controller. Exact duplicates are rejected; a greater
         * sequence for the same controller/tick replaces the earlier intent when that tick is consumed.
         */
        [[nodiscard]] Result<CharacterCommandAdmission> QueueMovementCommand(const CharacterMovementRequest &request);

        /**
         * @brief Freezes and schedules one exact next Character fixed tick on the owner thread.
         * @param input One-based next tick, exact scene generation and positive host fixed quantum.
         * @return Success or a typed affinity/lifecycle/order/request error without partial publication.
         * @post Commands are ordered by stable controller identity. A controller with no command performs no
         * movement for the tick; prior intent is never replayed. The queue closes before callbacks execute.
         */
        [[nodiscard]] Result<void> AdvanceFixedTick(const CharacterFixedTickInput &input);

        /** @brief Returns one coherent copy of the last published tick from any thread. */
        [[nodiscard]] CharacterPublishedTick PublishedTick() const noexcept;
        /** @brief Returns allocation-free cumulative command-pipeline counters. */
        [[nodiscard]] CharacterTickStatistics TickStatistics() const noexcept;

        /** @brief Closes admission and drains every controller record; safe repeatedly. */
        void Shutdown() noexcept;
        /** @brief Returns the current lifecycle state. @return Prepared, Active, or Destroyed. */
        [[nodiscard]] CharacterWorldState State() const noexcept;
        /** @brief Returns the immutable owner generations selected during preparation. @return Borrow valid for this world lifetime. */
        [[nodiscard]] const CharacterWorldDescriptor &Descriptor() const noexcept;
        /** @brief Returns the immutable settings snapshot retained for this world lifetime. @return Borrow valid for this world lifetime.
         */
        [[nodiscard]] const CharacterWorldSettings &Settings() const noexcept;
        /** @brief Returns the number of currently resident controllers. @return Live slot count; zero after shutdown. */
        [[nodiscard]] std::size_t ActiveControllerCount() const noexcept;
        /** @brief Returns the controller capacity reserved during preparation. @return Immutable slot count. */
        [[nodiscard]] std::size_t ControllerCapacity() const noexcept;

    private:
        struct Impl;
        /** @brief Takes a completely prepared implementation. */
        explicit CharacterWorld(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Character
