#pragma once

/** @file StreamingEvictionPolicy.h
 * @brief Bounded generation-fenced eviction selection and pin/lease conflict policy.
 */

#include "Horo/WorldStreaming/StreamingCellState.h"

#include <span>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct StreamingEvictionPolicyIdTag;
        struct StreamingEvictionPolicyRevisionTag;
        struct StreamingEvictionSnapshotRevisionTag;
    }  // namespace Detail

    /** @brief Stable host-owned eviction policy identity. */
    using StreamingEvictionPolicyId =
        Foundation::Detail::NonZeroId64<Detail::StreamingEvictionPolicyIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact immutable policy publication. */
    using StreamingEvictionPolicyRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingEvictionPolicyRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Authority publication advanced whenever residency, pins, leases or ranking facts change. */
    using StreamingEvictionSnapshotRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingEvictionSnapshotRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Admission gate; cancellation and shutdown use canonical retirement independently. */
    enum class StreamingEvictionLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count
    };

    /** @brief Inert immutable bounded selection policy owned by host composition. */
    struct StreamingEvictionPolicy final {
        static constexpr std::uint32_t CurrentContractVersion = 1;
        static constexpr std::uint32_t MaximumCandidateCount = 1024;
        std::uint32_t contractVersion{CurrentContractVersion};  /**< Exact in-memory contract version. */
        StreamingEvictionPolicyId id{};                         /**< Stable policy identity. */
        StreamingEvictionPolicyRevision revision{};             /**< Exact publication. */
        std::uint32_t maximumCandidates{MaximumCandidateCount}; /**< Positive bounded input ceiling. */
    };

    /** @brief Exact authority evidence captured atomically with all candidate facts. */
    struct StreamingEvictionContext final {
        StreamingRuntimeOwnerToken owner{};                                       /**< Current mounted authority lifetime. */
        StreamingEvictionPolicyId policy{};                                       /**< Expected policy identity. */
        StreamingEvictionPolicyRevision policyRevision{};                         /**< Expected publication. */
        StreamingEvictionSnapshotRevision snapshotRevision{};                     /**< Current residency/pin/lease publication. */
        std::uint64_t serviceTimeMilliseconds{};                                  /**< Unscaled monotonic observation time. */
        StreamingEvictionLifecycle lifecycle{StreamingEvictionLifecycle::Closed}; /**< Selection admission gate. */
    };

    /** @brief Value-owned exact attempt facts supplied by StreamingAuthorityRole; no borrowed leases or resources. */
    struct StreamingEvictionCandidate final {
        StreamingRuntimeOwnerToken owner{};                     /**< Exact owner that captured this row. */
        StreamingEvictionSnapshotRevision snapshotRevision{};   /**< Exact atomically captured authority publication. */
        StreamingFence fence{};                                 /**< Exact current cell generation. */
        StreamingCellState state{StreamingCellState::Unloaded}; /**< Canonical residency fact. */
        double retentionPriority{};                             /**< Finite non-negative aggregate priority; lower values retire first. */
        std::uint64_t lastUsedServiceMilliseconds{};            /**< Last use; older values retire first at equal priority. */
        std::uint32_t sourcePins{};                             /**< Owner-scoped source demands retaining residency. */
        std::uint32_t gameplayPins{};                           /**< Owner-scoped gameplay demands retaining residency. */
        std::uint32_t providerPins{};                           /**< Explicit provider retention demands. */
        std::uint32_t outstandingLeases{};                      /**< Read, worker, provider or GPU leases delaying physical reclamation. */
        [[nodiscard]] constexpr auto operator<=>(const StreamingEvictionCandidate &) const noexcept = default;
    };

    /** @brief Selected exact victim; selection authorizes a retirement request, never budget release or physical destruction. */
    struct StreamingEvictionVictim final {
        StreamingEvictionCandidate candidate{}; /**< Complete value-owned selection evidence. */

        /** @brief Checks captured lease evidence only; canonical retirement acknowledgements are always mandatory.
         * @return True when outstanding leases must drain before reclamation.
         */
        [[nodiscard]] bool RequiresLeaseDrain() const noexcept {
            return candidate.outstandingLeases != 0;
        }
    };

    /** @brief Selects unpinned Resident/Active attempts by ascending priority, oldest use and canonical cell tuple.
     * @param policy Immutable host policy.
     * @param context Current authority, snapshot and lifecycle facts.
     * @param candidates Complete immutable snapshot; duplicate cell identities are rejected even across generations.
     * @param output Caller-owned storage covering all candidate rows; only the selected prefix is written on success.
     * @return Selected victim count or typed invalid, unsupported, stale, capacity, conflict or lifecycle failure.
     * @details Allocates nothing. Validates every row before writing output. Loading, Evicting, Failed and Unloaded are excluded.
     * Pins always defeat pressure; leases delay reclamation without becoming residency pins. The authority must revalidate the exact
     * owner, snapshot publication and generation before beginning canonical retirement. New pins/leases revoke old proposals by
     * advancing the snapshot publication. Replacement routes old retirement to its original owner; selection never resets charges.
     * @post Failure leaves caller output unchanged; no residency, pin, lease or reservation state is mutated.
     */
    [[nodiscard]] Result<std::size_t> SelectStreamingEvictionVictims(const StreamingEvictionPolicy &policy,
                                                                     const StreamingEvictionContext &context,
                                                                     std::span<const StreamingEvictionCandidate> candidates,
                                                                     std::span<StreamingEvictionVictim> output);
}  // namespace Horo::WorldStreaming
