#pragma once

/** @file ReplicationSnapshotHistory.h
 * @brief Bounded connection-owned sent-state acknowledgements and expiring baseline pins.
 */

#include "Horo/Network/NetworkHandles.h"
#include "Horo/Network/ReplicationStateCodec.h"

namespace Horo::Network {
    /** @brief Overflow either clears history for a full-state retry or permanently closes admission. */
    enum class ReplicationHistoryOverflow : std::uint8_t {
        FullSnapshot,
        Disconnect
    };

    /** @brief Finite setup storage, retained-source charge and absolute lease duration in host ticks. */
    struct ReplicationHistoryLimits final {
        std::size_t maximumEntries{64};
        std::size_t maximumRetainedBytes{4 * 1024 * 1024};
        std::uint64_t leaseTicks{120};
        ReplicationHistoryOverflow overflow{ReplicationHistoryOverflow::FullSnapshot};
    };

    /** @brief Exact local routing fence; incarnation must never be reused for this connection/session. */
    struct ReplicationHistoryScope final {
        ConnectionHandle connection;
        NetworkSessionGeneration session;
        Runtime::SceneRuntimeId scene;
        std::uint64_t incarnation{};
        auto operator<=>(const ReplicationHistoryScope &) const noexcept = default;
    };

    /** @brief Exact selective acknowledgement; no cumulative or inferred receipt of earlier sends.
     * Local connection handles are routing metadata and are never serialized. The host resolves
     * the handle from the authenticated channel and preserves peer-echoed negotiated generation
     * evidence; it must never relabel an old acknowledgement with current session/Scene/incarnation.
     */
    struct ReplicationSnapshotAcknowledgement final {
        ReplicationHistoryScope scope;
        std::uint64_t sequence{};
        NetworkObjectId object;
        std::uint64_t publicationRevision{};
        auto operator<=>(const ReplicationSnapshotAcknowledgement &) const noexcept = default;
    };

    /** @brief Serial owner-thread history for one admitted connection and Scene incarnation.
     * Preparation allocates the entry window; operations do bounded scans with no new storage.
     * The host calls RetainSent after complete encoding and before submission, passes monotonically
     * increasing clock ticks, and closes this owner on disconnect or Scene replacement. No transport,
     * canonical owner access or Scene mutation occurs. Capture/world revocation also fences selection.
     * Returned baseline copies are short-lived encoding leases: release them before expiry/pressure
     * maintenance or lifecycle retirement. Storage charges conservatively count shared source backing
     * for each retained send, excluding separately budgeted capture pools and descriptor registries.
     */
    class ReplicationSnapshotHistory final {
    public:
        /** @brief Validates and reserves one finite history window.
         * @param scope Host-issued complete connection/Scene/session incarnation.
         * @param limits Explicit finite storage, lifetime and overflow policy.
         * @return Sole owner or typed state Invalid/Capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<ReplicationSnapshotHistory>> Create(ReplicationHistoryScope scope,
                                                                                        ReplicationHistoryLimits limits = {});
        ReplicationSnapshotHistory(const ReplicationSnapshotHistory &) = delete;
        ReplicationSnapshotHistory &operator=(const ReplicationSnapshotHistory &) = delete;
        /** @brief Reserves exact provenance and correlation for one completely encoded source projection.
         * @param sent Source plus exact codec projection, role and descriptor evidence used for encoding.
         * @param now Monotonic host tick; expiry is fixed at send and never refreshed by duplicate acks.
         * @param cancellation Checked before mutation; cancellation preserves prior history.
         * @return Selective ack correlation for outgoing framing (excluding the local handle), or typed error. The host
         * cancels the reservation when submission fails; only peer receipt enables baseline selection.
         * Capacity clears history: FullSnapshot requires re-encoding full state before retry;
         * Disconnect additionally closes this owner.
         * Counter exhaustion permanently closes admission; counters never wrap or reset after pressure.
         */
        [[nodiscard]] Result<ReplicationSnapshotAcknowledgement> RetainSent(ReplicationAcknowledgedBaseline sent, std::uint64_t now,
                                                                            const CancellationToken &cancellation = {});
        /** @brief Releases one reservation after failed/cancelled send submission, without sequence reuse.
         * @param token Exact locally issued send correlation.
         * @param now Monotonic host tick.
         * @return Success or typed identity/admission failure; no other send is cancelled.
         */
        [[nodiscard]] Result<void> CancelSent(const ReplicationSnapshotAcknowledgement &token, std::uint64_t now);
        /** @brief Marks only the exact retained send as received; duplicate receipt is idempotent.
         * @param ack Authenticated and host-routed selective acknowledgement.
         * @param now Monotonic host tick.
         * @param cancellation Checked before mutation.
         * @return Success or typed stale/expired/invalid/closed/cancelled failure.
         */
        [[nodiscard]] Result<void> Acknowledge(const ReplicationSnapshotAcknowledgement &ack, std::uint64_t now,
                                               const CancellationToken &cancellation = {});
        /** @brief Selects the newest acknowledged compatible source or an empty full-state fallback.
         * @param source Current exact source occurrence to encode.
         * @param role Current recipient role revision.
         * @param generation Current negotiated descriptor generation.
         * @param projection Current codec projection fingerprint.
         * @param now Monotonic host tick.
         * @param cancellation Checked before mutation.
         * @return Short-lived baseline copy or typed admission failure; missing/lost/expired roots are empty.
         */
        [[nodiscard]] Result<ReplicationAcknowledgedBaseline> Baseline(const ReplicationCapturedStatePin &source,
                                                                       ReplicationRoleRevision role, std::uint64_t generation,
                                                                       const Sha256Digest &projection, std::uint64_t now,
                                                                       const CancellationToken &cancellation = {});
        /** @brief Releases all retained pins for memory pressure without reusing sequence identities.
         * @return Success or typed owner-thread/closed failure. Next selection requires full state.
         */
        [[nodiscard]] Result<void> ReleaseForMemoryPressure();
        /** @brief Releases expired and revoked pins during the host's bounded connection poll.
         * @param now Monotonic host tick, including idle/loss periods without sends.
         * @param cancellation Checked before mutation.
         * @return Success or typed admission failure.
         */
        [[nodiscard]] Result<void> Expire(std::uint64_t now, const CancellationToken &cancellation = {});
        /** @brief Permanently clears all pins on disconnect, Scene replacement or shutdown.
         * @return Success on repeated owner-thread calls, otherwise typed affinity failure.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct Entry final {
            ReplicationSnapshotAcknowledgement token;
            ReplicationAcknowledgedBaseline baseline;
            std::uint64_t expires{};
            std::size_t bytes{};
            bool acknowledged{};
        };

        ReplicationSnapshotHistory(ReplicationHistoryScope scope, ReplicationHistoryLimits limits);
        /** @brief Checks admission and monotonic time before pruning expired or revoked pins. */
        [[nodiscard]] Result<void> Advance(std::uint64_t now, const CancellationToken &cancellation);
        /** @brief Checks exact captured session/Scene authority eligibility. */
        [[nodiscard]] bool Current(const ReplicationCapturedStatePin &state) const noexcept;
        /** @brief Clears owned pins and charges, preserving sequence and clock high-water marks. */
        void Clear() noexcept;
        ReplicationHistoryScope scope_;
        ReplicationHistoryLimits limits_;
        std::thread::id owner_;
        std::vector<Entry> entries_;
        std::size_t bytes_{};
        std::uint64_t sequence_{};
        std::uint64_t clock_{};
        bool closed_{};
    };
}  // namespace Horo::Network
