#pragma once

/**
 * @file ReplicationRoles.h
 * @brief Session-pinned replication roles, routing conditions, and safe-point ownership changes.
 */

#include "Horo/Network/NetworkObjectIdentity.h"
#include "Horo/Network/ReplicationDescriptor.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <thread>

namespace Horo::Network {
    struct NetworkSessionGenerationTag;
    struct NetworkPeerIdentityTag;
    struct ReplicationRoleRevisionTag;

    /** @brief Non-zero active network-session generation. */
    using NetworkSessionGeneration = ReplicationIdentity<NetworkSessionGenerationTag, std::uint64_t>;
    /** @brief Non-zero peer identity scoped by an active session generation. */
    using NetworkPeerId = ReplicationIdentity<NetworkPeerIdentityTag, std::uint64_t>;
    /** @brief Monotonic publication revision of one object role binding. */
    using ReplicationRoleRevision = ReplicationIdentity<ReplicationRoleRevisionTag, std::uint64_t>;

    /** @brief Authority-issued ownership fact for the exact initial object occurrence. */
    struct ReplicationInitialOwnership final {
        NetworkObjectId object;
        ReplicationRoleRevision revision;
        std::optional<NetworkPeerId> autonomousOwner;

        auto operator<=>(const ReplicationInitialOwnership &) const noexcept = default;
    };

    /** @brief Explicit world role; process locality and presentation ownership never imply authority. */
    enum class ReplicationExecutionRole : std::uint8_t {
        Standalone,
        AuthorityServer,
        AutonomousClient,
        SimulatedClient,
        Count,
    };

    /** @brief Record phase used by InitialOnly visibility evaluation. */
    enum class ReplicationRecordKind : std::uint8_t {
        Spawn,
        Update,
        Count,
    };

    /** @brief Immutable session/object/schema-pinned role and autonomous-owner evidence. */
    struct ReplicationRoleBinding final {
        ReplicationRoleRevision revision;             /**< Exact role publication revision. */
        NetworkSessionGeneration session;             /**< Exact active session generation. */
        NetworkObjectId object;                       /**< Exact authority-epoch-scoped object occurrence. */
        ReplicationSchemaId schema;                   /**< Exact stable payload schema identity. */
        ReplicationSchemaVersion schemaVersion;       /**< Exact negotiated payload schema version. */
        ReplicationExecutionRole role{};              /**< Explicit local world role. */
        std::optional<NetworkPeerId> localPeer;       /**< Client peer represented by this binding, if any. */
        std::optional<NetworkPeerId> autonomousOwner; /**< Current autonomous owner, if granted. */

        /** @brief Validates role, peer, session, object, and schema invariants. @return Whether the binding is coherent. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const ReplicationRoleBinding &) const noexcept = default;
    };

    /** @brief One complete next role/ownership publication staged for an owner safe point. */
    struct ReplicationRoleTransition final {
        ReplicationRoleRevision expectedRevision; /**< Exact current revision the command was based on. */
        ReplicationRoleBinding next;              /**< Complete next immutable binding. */
    };

    /** @brief One owner-computed bounded decision for an exact registered custom condition. */
    struct ReplicationCustomConditionEvidence final {
        ReplicationConditionId condition; /**< Exact descriptor policy identity. */
        bool visible{};                   /**< Owner-computed visibility decision. */
    };

    /**
     * @brief Evaluates one declared field visibility condition for a pinned recipient binding.
     * @param field Stable schema-owned field descriptor.
     * @param recipient Exact client role and ownership evidence for one session/object/schema version.
     * @param record Spawn or update record being projected.
     * @param customEvidence Exact custom-policy result; absent for built-in conditions.
     * @return Visibility decision or typed malformed-context failure.
     */
    [[nodiscard]] Result<bool> EvaluateReplicationCondition(
        const ReplicationFieldDescriptor &field, const ReplicationRoleBinding &recipient, ReplicationRecordKind record,
        std::optional<ReplicationCustomConditionEvidence> customEvidence = std::nullopt);

    /**
     * @brief Authorizes canonical field origination without treating client ownership as server authority.
     * @param field Stable schema-owned field descriptor.
     * @param writer Exact pinned world role attempting to originate canonical state.
     * @return Success only for an authority-server binding, otherwise a typed denial or malformed-context failure.
     */
    [[nodiscard]] Result<void> AuthorizeReplicationFieldWrite(const ReplicationFieldDescriptor &field,
                                                              const ReplicationRoleBinding &writer);

    /** @brief Owner-thread stage/commit boundary for one bounded role and ownership binding. */
    class ReplicationRoleState final {
    public:
        /**
         * @brief Creates active state owned by the calling thread.
         * @param initial Complete initial binding.
         * @return Active state or typed malformed-context failure.
         */
        [[nodiscard]] static Result<ReplicationRoleState> Create(const ReplicationRoleBinding &initial);

        ReplicationRoleState(const ReplicationRoleState &) = delete;
        ReplicationRoleState &operator=(const ReplicationRoleState &) = delete;
        ReplicationRoleState(ReplicationRoleState &&) noexcept = default;
        ReplicationRoleState &operator=(ReplicationRoleState &&) noexcept = default;

        /**
         * @brief Stages one complete transition without changing the published binding.
         * @param transition Expected-current and complete next binding.
         * @return Success or typed stale, pending, affinity, lifecycle, or malformed failure.
         */
        [[nodiscard]] Result<void> Stage(const ReplicationRoleTransition &transition);

        /**
         * @brief Publishes the exact staged revision at the runtime owner safe point.
         * @param revision Exact staged next revision.
         * @return Success or typed stale, affinity, or lifecycle failure.
         */
        [[nodiscard]] Result<void> CommitAtSafePoint(ReplicationRoleRevision revision);

        /** @brief Returns an owned copy of the current binding on the owner thread. @return Current binding or typed failure. */
        [[nodiscard]] Result<ReplicationRoleBinding> Snapshot() const;

        /**
         * @brief Idempotently rejects later work and clears an uncommitted transition on the owner thread.
         * @return Success or typed affinity failure.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        explicit ReplicationRoleState(const ReplicationRoleBinding &initial) noexcept;

        std::thread::id ownerThread_;
        ReplicationRoleBinding current_;
        std::optional<ReplicationRoleBinding> pending_;
        bool stopped_{};
    };
}  // namespace Horo::Network
