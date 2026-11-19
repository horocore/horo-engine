#pragma once

/** @file NetworkInitialStateStream.h @brief Bounded reliable transfer of one complete initial replication snapshot. */

#include "Horo/Network/ReplicationInboundApply.h"

namespace Horo::Network {
    inline constexpr std::size_t MaximumNetworkInitialStateFrameBytes = 1024;
    inline constexpr std::size_t NetworkInitialStateSegmentHeaderBytes = 64;
    inline constexpr std::size_t MaximumNetworkInitialStateWireBytes = 64 * 1024 * 1024;

    /** @brief Binds the client's successful publication to an exact server snapshot. */
    struct NetworkInitialStateStamp final {
        NetworkSessionGeneration session;
        ReplicationAuthorityEpoch authority;
        std::uint64_t tick{};

        auto operator<=>(const NetworkInitialStateStamp &) const noexcept = default;
    };

    /** @brief Reads an admitted frame's world scope without retaining its payload. @return Exact stamp or a typed framing error. */
    [[nodiscard]] Result<NetworkInitialStateStamp> ReadNetworkInitialStateStamp(std::span<const std::byte> payload);
    /** @brief Encodes the publication receipt on the initial-state route. @return Owned bounded receipt or a typed framing error. */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNetworkInitialStateReceipt(NetworkInitialStateStamp stamp);
    /** @brief Decodes a receipt with an exact fixed representation. @return Its stamp or a typed framing error. */
    [[nodiscard]] Result<NetworkInitialStateStamp> DecodeNetworkInitialStateReceipt(std::span<const std::byte> payload);

    /** @brief Strongly retains the local Scene transaction owner through initial field publication. */
    struct NetworkInitialStateTarget final {
        std::shared_ptr<IReplicationApplyOwner> owner;
        std::shared_ptr<ReplicationInboundApply> receiver;
    };

    /** @brief Projects an authority ownership fact against the exact recipient without granting authority.
     * @param ownership Ownership metadata published by the authority on the initial snapshot route.
     * @param mapping Exact resolved Scene object and schema.
     * @param session Receiving world generation.
     * @param recipient Bound identity of the local or selected remote replica.
     * @return Complete autonomous or simulated binding, or a typed stale-identity failure. */
    [[nodiscard]] Result<ReplicationRoleBinding> ProjectInitialReplicationRole(const ReplicationInitialOwnership &ownership,
                                                                               const NetworkObjectMappingEntry &mapping,
                                                                               NetworkSessionGeneration session, NetworkPeerId recipient);

    /** @brief Owner-thread application adapters for declared, recipient-specific field capture and transactional apply.
     * @details These synchronous callbacks must not reenter the network controller or retain borrowed world/peer references. */
    class INetworkInitialStateHost {
    public:
        virtual ~INetworkInitialStateHost() = default;
        /** @brief Resolves the local shadow-world recipient from the current admitted connection.
         * @param peer Exact authenticated server channel.
         * @return Local recipient or typed incompatibility. */
        [[nodiscard]] virtual Result<NetworkPeerId> ClientRecipient(const PeerSessionLifecycle &peer) = 0;
        /** @brief Supplies a complete, committed capture for a remote client, or pending while capture catches up.
         * @param world Borrowed exact authority mapping for this call.
         * @param lifecycle Borrowed read-only lifecycle for acquiring capture evidence at the owner's committed tick.
         * @param peer Exact authenticated target client channel.
         * @param maximumWireBytes Total field record budget.
         * @return Complete recipient snapshot, pending or typed failure. */
        [[nodiscard]] virtual Result<std::optional<ReplicationInitialStateSnapshot>> Capture(const ReplicationWorldReadLease &world,
                                                                                             const ReplicationWorldLifecycle &lifecycle,
                                                                                             const PeerSessionLifecycle &peer,
                                                                                             std::size_t maximumWireBytes) = 0;
        /** @brief Builds the apply owner and binds every object in the exact published replica mapping.
         * @param authority Exact authenticated local receiver grant.
         * @param world Borrowed receiving lifecycle; the controller retires the target first.
         * @param ownership Ordered exact object ownership facts, borrowed only during preparation.
         * @param now Current admission clock.
         * @return Complete target or typed incompatibility.
         * @details Keep new ownership detached until the same apply transaction commits its field values. */
        [[nodiscard]] virtual Result<NetworkInitialStateTarget> PrepareReceiver(const ReplicationInboundAuthority &authority,
                                                                                ReplicationWorldLifecycle &world,
                                                                                std::span<const ReplicationInitialOwnership> ownership,
                                                                                std::uint64_t now) = 0;
    };

    /**
     * @brief Encodes the complete expected snapshot size and authoritative tick.
     * @param snapshot Complete canonical initial records in object order.
     * @return Owned manifest payload or a typed invalid or capacity error.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeNetworkInitialStateManifest(const ReplicationInitialStateSnapshot &snapshot);

    /** @brief One owned bounded segment and the exact next unsent position within its record. */
    struct NetworkInitialStateSegment final {
        std::vector<std::byte> payload;
        std::size_t nextOffset{};
    };

    /**
     * @brief Copies the next bounded bytes of one canonical record for the reliable Control outbox.
     * @param snapshot Whole committed initial snapshot containing the record.
     * @param index Zero-based record index in the manifest.
     * @param offset First unsent byte in this record.
     * @param maximumPayloadBytes Selected codec and transport payload budget, including the segment header.
     * @return Exact payload and the next offset, or a typed invalid or capacity error.
     */
    [[nodiscard]] Result<NetworkInitialStateSegment> EncodeNetworkInitialStateSegment(const ReplicationInitialStateSnapshot &snapshot,
                                                                                      std::size_t index, std::size_t offset,
                                                                                      std::size_t maximumPayloadBytes);

    /** @brief One owner-thread admitted Control receiver. It assembles bytes without invoking a field serializer. */
    class NetworkInitialStateInbox final : public IInboundMessageHandler {
    public:
        /**
         * @brief Binds the inbox to one already authenticated server connection.
         * @param authority Exact verified server peer, route and world occurrence.
         * @param message Exact registered Control identity, including schema and version.
         * @param maximumWireBytes Complete snapshot storage allowance.
         */
        NetworkInitialStateInbox(ReplicationInboundAuthority authority, MessageIdentityDescriptor message,
                                 std::size_t maximumWireBytes) noexcept;
        NetworkInitialStateInbox(const NetworkInitialStateInbox &) = delete;
        NetworkInitialStateInbox &operator=(const NetworkInitialStateInbox &) = delete;

        /** @copydoc IInboundMessageHandler::Handle */
        [[nodiscard]] Result<void> Handle(const MessageEnvelope &message) override;
        /** @copydoc IInboundMessageHandler::HandleAdmitted */
        [[nodiscard]] Result<void> HandleAdmitted(const InboundMessageContext &context, const MessageEnvelope &message) override;

        /**
         * @brief Moves one complete snapshot to the field decoder exactly once.
         * @param now Current owner admission clock tick.
         * @return Complete snapshot or a typed pending, canceled, or stale error.
         */
        [[nodiscard]] Result<ReplicationInitialStateSnapshot> Take(std::uint64_t now);

        /**
         * @brief Transfers and publishes the complete authenticated snapshot at the structural safe point.
         * @param receiver Initial snapshot receiver for the same authority, world and local recipient.
         * @param request Exact receiving world, nonzero local simulation tick, and structural phase.
         * @param now Current owner admission-clock tick.
         * @return Pending while incomplete, committed report, or typed failure with no partial state.
         */
        [[nodiscard]] Result<ReplicationApplyReport> ApplyCompleted(ReplicationInboundApply &receiver,
                                                                    const ReplicationWorldWorkRequest &request, std::uint64_t now);
        /** @brief Reports that all declared bytes have arrived. @return True only before the snapshot is transferred. */
        [[nodiscard]] bool Ready() const noexcept;
        /** @brief Reports the required committed object count after all wire bytes have arrived.
         * @return Exact declared record count, or no count while receipt is pending or retired. */
        [[nodiscard]] std::optional<std::size_t> ReadyRecordCount() const noexcept;
        /** @brief Copies the immutable ownership facts of a complete admitted stream.
         * @param now Current owner admission-clock tick.
         * @return Ordered facts or a typed pending/capacity error. */
        [[nodiscard]] Result<std::vector<ReplicationInitialOwnership>> ReadOwnership(std::uint64_t now) const;

    private:
        /** @brief Verifies the authenticated connection and exact reserved Control route. */
        [[nodiscard]] Result<void> Admit(const InboundMessageContext &context, const MessageEnvelope &message) const;
        /** @brief Opens one bounded inventory before accepting segments. */
        [[nodiscard]] Result<void> Begin(std::uint64_t tick, std::span<const std::byte> body);
        /** @brief Adds the next contiguous slice to the current record. */
        [[nodiscard]] Result<void> Append(std::uint64_t tick, std::span<const std::byte> body);
        /** @brief Opens a new record or checks the next exact offset of an in-progress record. */
        [[nodiscard]] Result<void> PrepareRecord(const ReplicationInitialOwnership &ownership, std::size_t total, std::size_t offset);
        /** @brief Reserves one record within the remaining manifest budget. */
        [[nodiscard]] Result<void> StartRecord(const ReplicationInitialOwnership &ownership, std::size_t bytes);
        /** @brief Checks that the complete record and byte inventories arrived. */
        [[nodiscard]] bool Complete() const noexcept;

        ReplicationInboundAuthority authority_;
        MessageIdentityDescriptor message_;
        std::size_t maximumWireBytes_{};
        ReplicationInitialStateSnapshot snapshot_;
        std::size_t expectedRecords_{};
        std::size_t remainingBytes_{};
        std::size_t pendingBytes_{};
        CancellationToken delivery_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool begun_{};
        bool transferred_{};
    };
}  // namespace Horo::Network
