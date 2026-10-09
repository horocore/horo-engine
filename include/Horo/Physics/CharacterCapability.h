#pragma once

/** @file CharacterCapability.h
 * @brief Revocable Physics-owned Character creation, command and copied-query access.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Physics/CharacterCommandPipeline.h"

#include <memory>

namespace Horo::Character {
    class CharacterWorld;
    struct CharacterCapabilityState;

    /** @brief Finite number of simultaneously retained client grants per Character world. */
    inline constexpr std::uint32_t MaximumCharacterCapabilitiesPerWorld = 256;

    /** @brief Immutable routing evidence; identity alone grants no access. */
    struct CharacterCapabilityIdentity final {
        std::uint64_t sceneGeneration{};
        CharacterWorldId world;
        Physics::PhysicsWorldId physicsWorld;
        std::uint64_t generation{};
    };

    /**
     * @brief Copyable, independently revocable client without world or backend ownership.
     * @details All operations except Identity and Revoke run on the world's preparation thread.
     * The owner issues grants explicitly; this class makes no module permission decision. It
     * exposes neither tick advancement nor Physics queries, mutable state, native objects or
     * world discovery. Clients and owned query results may outlive the world. World destruction
     * remains owner-thread-only and cannot race an operation; retirement fences every client
     * before storage release. Copying a client shares its grant, not a new authority.
     */
    class CharacterCapability final {
    public:
        /** @brief Constructs an inert client whose operations return CapabilityUnavailable. */
        CharacterCapability() noexcept = default;
        /** @brief Returns copied routing evidence from any thread. @return Invalid identity for an inert/moved-from client. */
        [[nodiscard]] CharacterCapabilityIdentity Identity() const noexcept;
        /**
         * @brief Installs a controller only during the prepared owner phase.
         * @param descriptor Owned inert descriptor matching the issued scene, Character and Physics worlds.
         * @return Stable controller handle or the original typed descriptor/capacity/lifecycle failure.
         * @details Active creation and reentrant placement/tick callbacks return InvalidState without mutation.
         */
        [[nodiscard]] Result<CharacterControllerHandle> CreateController(const CharacterControllerDescriptor &descriptor) const;
        /**
         * @brief Copies a tick-addressed intent into the world's existing bounded admission queue.
         * @param request Owned handle, future tick, producer sequence and movement intent.
         * @return Deferred/full/busy admission or the original typed handle/order/request failure.
         * @details Revocation before command closure discards this grant's queued commands. Once
         * frozen, an intent belongs to that attempted tick and revocation does not rewrite its
         * simulation outcome. No client or caller storage is retained by the queue.
         */
        [[nodiscard]] Result<CharacterCommandAdmission> QueueMovementCommand(const CharacterMovementRequest &request) const;
        /** @brief Copies an inert resident descriptor. @param handle Exact live handle. @return Owned value or typed handle failure. */
        [[nodiscard]] Result<CharacterControllerDescriptor> ControllerDescriptor(const CharacterControllerHandle &handle) const;
        /** @brief Copies the committed root. @param handle Exact spawned handle. @return Owned publication or typed lifecycle failure. */
        [[nodiscard]] Result<CharacterTransformPublication> ControllerTransform(const CharacterControllerHandle &handle) const;
        /**
         * @brief Copies the last complete immutable movement publication, preserving tick/sequence/revisions.
         * @param handle Exact spawned handle with a completed movement tick.
         * @return Owned bounded snapshot or typed handle/lifecycle failure; never a manager-storage borrow.
         */
        [[nodiscard]] Result<CharacterLocomotionSnapshot> ControllerLocomotionSnapshot(const CharacterControllerHandle &handle) const;
        /** @brief Permanently closes this shared grant from any thread; idempotent and invokes no world or consumer callback. */
        void Revoke() const noexcept;

    private:
        friend class CharacterWorld;
        explicit CharacterCapability(std::shared_ptr<CharacterCapabilityState> state) noexcept;
        std::shared_ptr<CharacterCapabilityState> state_;
    };
}  // namespace Horo::Character
