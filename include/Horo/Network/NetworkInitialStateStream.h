#pragma once

/** @file NetworkInitialStateStream.h @brief Bounded reliable transfer of one complete initial replication snapshot. */

#include "Horo/Network/ReplicationInboundApply.h"

namespace Horo::Network {
    inline constexpr std::size_t MaximumNetworkInitialStateFrameBytes = 1024;
    inline constexpr std::size_t NetworkInitialStateSegmentHeaderBytes = 48;

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

    private:
        /** @brief Verifies the authenticated connection and exact reserved Control route. */
        [[nodiscard]] Result<void> Admit(const InboundMessageContext &context, const MessageEnvelope &message) const;
        /** @brief Opens one bounded inventory before accepting segments. */
        [[nodiscard]] Result<void> Begin(std::uint64_t tick, std::span<const std::byte> body);
        /** @brief Adds the next contiguous slice to the current record. */
        [[nodiscard]] Result<void> Append(std::uint64_t tick, std::span<const std::byte> body);
        /** @brief Opens a new record or checks the next exact offset of an in-progress record. */
        [[nodiscard]] Result<void> PrepareRecord(NetworkObjectId object, std::size_t total, std::size_t offset);
        /** @brief Reserves one record within the remaining manifest budget. */
        [[nodiscard]] Result<void> StartRecord(NetworkObjectId object, std::size_t bytes);
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
