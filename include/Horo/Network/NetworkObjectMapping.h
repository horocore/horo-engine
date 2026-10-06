#pragma once

/**
 * @file NetworkObjectMapping.h
 * @brief Session-owned mapping between replicated-object occurrences and runtime-scene entities.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkObjectIdentity.h"
#include "Horo/Network/ReplicationIdentity.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Network {
    /** @brief Explicit owner-thread lifecycle of one session-owned mapping. */
    enum class NetworkObjectMappingState : std::uint8_t {
        Active,
        ShuttingDown,
        SceneInvalidated,
        Count
    };

    /**
     * @brief Stable declaration provenance kept separate from a session occurrence identity.
     *
     * An authored object is optional because runtime-spawned objects have no authored SceneObjectId. Neither field is used as
     * NetworkObjectId, ECS storage identity, or mapping lookup precedence.
     */
    struct NetworkObjectProvenance final {
        ReplicationSchemaId schema;
        ReplicationSchemaVersion schemaVersion;
        std::optional<Runtime::SceneObjectId> authoredObject;

        /** @brief Validates stable schema and optional authored identity. @return Whether all present identities are valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return schema.IsValid() && schemaVersion.IsValid() && (!authoredObject.has_value() || authoredObject->IsValid());
        }

        constexpr auto operator<=>(const NetworkObjectProvenance &) const noexcept = default;
    };

    /** @brief Complete live association copied into immutable diagnostic evidence. */
    struct NetworkObjectMappingEntry final {
        NetworkObjectId object;             /**< Exact authority-epoch-scoped occurrence. */
        Runtime::EntityRef entity;          /**< Exact scene and ECS slot generation. */
        NetworkObjectProvenance provenance; /**< Stable schema/authored origin, never lookup identity. */

        constexpr auto operator<=>(const NetworkObjectMappingEntry &) const noexcept = default;
    };

    /** @brief Owned immutable diagnostic projection detached from later mapping mutation or destruction. */
    class NetworkObjectMappingSnapshot final {
    public:
        /** @brief Returns the captured authority epoch. @return Exact session epoch at capture. */
        [[nodiscard]] constexpr ReplicationAuthorityEpoch Epoch() const noexcept {
            return epoch_;
        }

        /** @brief Returns the captured runtime scene. @return Exact scene instance at capture. */
        [[nodiscard]] constexpr Runtime::SceneRuntimeId Scene() const noexcept {
            return scene_;
        }

        /** @brief Returns the captured owner lifecycle. @return Exact state at capture. */
        [[nodiscard]] constexpr NetworkObjectMappingState State() const noexcept {
            return state_;
        }

        /**
         * @brief Views this snapshot's owned entries in ascending object-slot order.
         * @return Borrowed immutable view valid only while this snapshot value remains alive and unmoved.
         */
        [[nodiscard]] std::span<const NetworkObjectMappingEntry> Entries() const noexcept {
            return entries_;
        }

    private:
        friend class NetworkObjectMapping;

        NetworkObjectMappingSnapshot(ReplicationAuthorityEpoch epoch, Runtime::SceneRuntimeId scene, NetworkObjectMappingState state,
                                     std::vector<NetworkObjectMappingEntry> entries) noexcept;

        ReplicationAuthorityEpoch epoch_;
        Runtime::SceneRuntimeId scene_;
        NetworkObjectMappingState state_{NetworkObjectMappingState::SceneInvalidated};
        std::vector<NetworkObjectMappingEntry> entries_;
    };

    /**
     * @brief Bounded owner-thread map for one session authority epoch and one exact runtime scene.
     *
     * The map performs no Scene mutation, ECS scanning, callback invocation, transport work, ambient registration, or internal
     * synchronization. Its session owner serializes every call at a declared safe point. Retired slots retain their last generation;
     * reuse admits only the exact next generation and never wraps. Creation reserves both sorted indexes, so registration and lookup
     * do not allocate. Shutdown and scene invalidation are terminal.
     */
    class NetworkObjectMapping final {
    public:
        /**
         * @brief Prepares one bounded session mapping.
         * @param epoch Non-zero authority epoch unique to the session/world authority generation.
         * @param scene Exact non-zero runtime-scene instance owned by this mapping.
         * @param maximumSlots Maximum distinct object slots, including retired tombstones.
         * @return Prepared mapping or a typed invalid/capacity error.
         */
        [[nodiscard]] static Result<NetworkObjectMapping> Create(ReplicationAuthorityEpoch epoch, Runtime::SceneRuntimeId scene,
                                                                 std::size_t maximumSlots);

        /**
         * @brief Registers one live occurrence without mutating Scene.
         * @param entry Exact object/entity generations and stable declaration provenance.
         * @return Success or typed malformed, conflict, stale-generation, capacity, or terminal error.
         */
        [[nodiscard]] Result<void> Register(const NetworkObjectMappingEntry &entry);

        /** @brief Resolves an exact live occurrence. @param object Exact object generation. @return Exact EntityRef or unknown error. */
        [[nodiscard]] Result<Runtime::EntityRef> Resolve(NetworkObjectId object) const;

        /** @brief Copies one exact live occurrence without constructing a mapping snapshot.
         * @param object Exact authority/slot/generation occurrence.
         * @return Owned entry or the original lifecycle/identity error.
         */
        [[nodiscard]] Result<NetworkObjectMappingEntry> ResolveEntry(NetworkObjectId object) const;

        /** @brief Reverse-resolves one exact live entity generation. @param entity Scene-qualified entity. @return Object or unknown error.
         */
        [[nodiscard]] Result<NetworkObjectId> Find(Runtime::EntityRef entity) const;

        /**
         * @brief Retires one exact live occurrence while preserving its slot generation tombstone.
         * @param object Exact current object identity.
         * @return Success or unknown/terminal error; repeated or stale retirement never affects a newer occurrence.
         */
        [[nodiscard]] Result<void> Retire(NetworkObjectId object);

        /**
         * @brief Terminally invalidates all mappings when their exact scene is destroyed or replaced.
         * @param scene Exact scene being invalidated; another scene cannot invalidate this owner.
         * @return Success, invalid-scene error, or terminal error.
         */
        [[nodiscard]] Result<void> InvalidateScene(Runtime::SceneRuntimeId scene);

        /** @brief Enters terminal shutdown and rejects later lookup or mutation. */
        void BeginShutdown() noexcept;

        /** @brief Captures owned immutable live-entry evidence. @return Snapshot or typed allocation failure. */
        [[nodiscard]] Result<NetworkObjectMappingSnapshot> Snapshot() const;

        /** @brief Returns the current owner lifecycle. @return Active or a terminal state. */
        [[nodiscard]] constexpr NetworkObjectMappingState State() const noexcept {
            return state_;
        }

        /** @brief Returns the live mapping count. @return Number of active associations. */
        [[nodiscard]] constexpr std::size_t Size() const noexcept {
            return liveCount_;
        }

        NetworkObjectMapping(NetworkObjectMapping &&) noexcept = default;
        NetworkObjectMapping &operator=(NetworkObjectMapping &&) noexcept = default;
        NetworkObjectMapping(const NetworkObjectMapping &) = delete;
        NetworkObjectMapping &operator=(const NetworkObjectMapping &) = delete;

    private:
        struct SlotRecord final {
            std::uint64_t slot{};
            std::uint32_t generation{};
            std::optional<NetworkObjectMappingEntry> live;
        };

        struct EntityRecord final {
            Runtime::EntityRef entity;
            NetworkObjectId object;
        };

        NetworkObjectMapping(ReplicationAuthorityEpoch epoch, Runtime::SceneRuntimeId scene, std::size_t maximumSlots,
                             std::vector<SlotRecord> slots, std::vector<EntityRecord> entityIndex) noexcept;

        [[nodiscard]] std::vector<SlotRecord>::iterator LowerBound(std::uint64_t slot) noexcept;
        [[nodiscard]] std::vector<SlotRecord>::const_iterator LowerBound(std::uint64_t slot) const noexcept;
        [[nodiscard]] std::vector<EntityRecord>::iterator LowerBound(Runtime::EntityRef entity) noexcept;
        [[nodiscard]] std::vector<EntityRecord>::const_iterator LowerBound(Runtime::EntityRef entity) const noexcept;
        [[nodiscard]] Result<void> ValidateRegistration(const NetworkObjectMappingEntry &entry) const;
        [[nodiscard]] Result<void> ReconcileSlot(const NetworkObjectMappingEntry &entry);
        [[nodiscard]] Result<void> RequireActive() const;

        ReplicationAuthorityEpoch epoch_;
        Runtime::SceneRuntimeId scene_;
        std::size_t maximumSlots_{};
        std::vector<SlotRecord> slots_;
        std::vector<EntityRecord> entityIndex_;
        std::size_t liveCount_{};
        NetworkObjectMappingState state_{NetworkObjectMappingState::SceneInvalidated};
    };
}  // namespace Horo::Network
