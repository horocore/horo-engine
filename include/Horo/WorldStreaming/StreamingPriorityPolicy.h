#pragma once

/**
 * @file StreamingPriorityPolicy.h
 * @brief Deterministic distance, priority, queue-age, and stable-tie ranking policy.
 */

#include "Horo/WorldStreaming/StreamingSourceDescriptor.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct StreamingPriorityPolicyIdTag;
        struct StreamingPriorityPolicyRevisionTag;
    }  // namespace Detail

    /** @brief Stable identity of one host-owned world-streaming priority policy. */
    using StreamingPriorityPolicyId =
        Foundation::Detail::NonZeroId64<Detail::StreamingPriorityPolicyIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact immutable publication revision of one priority policy. */
    using StreamingPriorityPolicyRevision =
        Foundation::Detail::NonZeroId64<Detail::StreamingPriorityPolicyRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Lifecycle gate captured by one immutable ranking pass. */
    enum class StreamingPriorityPolicyState : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count,
    };

    /** @brief Versioned host-authored priority policy with bounded numerical inputs. */
    struct StreamingPriorityPolicyRequest final {
        static constexpr std::uint32_t CurrentContractVersion = 1;
        static constexpr std::uint32_t MaximumCandidateCount = 65'536;

        std::uint32_t contractVersion{CurrentContractVersion}; /**< Exact supported in-memory contract version. */
        StreamingPriorityPolicyId id{};                        /**< Stable policy authority identity. */
        StreamingPriorityPolicyRevision revision{};            /**< Exact immutable policy publication. */
        std::uint64_t epsilonMillimeters{1'000};               /**< Positive distance denominator floor; default one metre. */
        std::array<double, static_cast<std::size_t>(StreamingSourceIntent::Count)>
            intentMultipliers{1.0, 0.9, 0.8, 1.2}; /**< Camera, Gameplay, Network, Preload weights. */
        double ageSlopePerSecond{0.1};             /**< Non-negative priority gained per queued second. */
        double maximumAgeBoost{2.0};               /**< Finite non-negative queue-age contribution cap. */
        std::uint32_t maximumCandidates{1'024};    /**< Maximum rows ranked by one bounded call. */
    };

    /** @brief Immutable validated priority policy; construction has no registration or activation effects. */
    class StreamingPriorityPolicy final {
    public:
        /**
         * @brief Validates and captures one complete priority policy.
         * @param request Versioned policy request.
         * @return Immutable policy or a typed invalid, unsupported, or capacity failure.
         */
        [[nodiscard]] static Result<StreamingPriorityPolicy> Create(const StreamingPriorityPolicyRequest &request);

        /** @brief Returns the stable policy authority. @return Non-zero identity. */
        [[nodiscard]] StreamingPriorityPolicyId Id() const noexcept;
        /** @brief Returns the exact policy publication. @return Non-zero immutable revision. */
        [[nodiscard]] StreamingPriorityPolicyRevision Revision() const noexcept;
        /** @brief Returns the admitted row ceiling. @return Positive implementation-bounded count. */
        [[nodiscard]] std::uint32_t MaximumCandidates() const noexcept;

    private:
        explicit StreamingPriorityPolicy(const StreamingPriorityPolicyRequest &request) noexcept;

        friend Result<std::size_t> RankStreamingCellPriorities(const StreamingPriorityPolicy &,
                                                               const struct StreamingPriorityEvaluationContext &,
                                                               std::span<const struct StreamingCellPriorityCandidate>,
                                                               std::span<struct StreamingRankedCellPriority>);

        StreamingPriorityPolicyRequest request_{};
    };

    /** @brief One immutable cell-work candidate evaluated against an exact source revision. */
    struct StreamingCellPriorityCandidate final {
        StreamingSourceDescriptor source{};          /**< Exact source identity, intent, base priority, owner, and revision. */
        StreamingCellId cell{};                      /**< Stable cell tuple used as the primary equal-score tie break. */
        std::uint64_t distanceMillimeters{};         /**< Non-negative exact host-declared distance to the source relevance origin. */
        double priorityOverride{1.0};                /**< Explicit multiplier in the closed [0.5, 2.0] range. */
        std::uint64_t queuedAtServiceMilliseconds{}; /**< Unscaled monotonic service timestamp captured at enqueue. */
    };

    /** @brief Immutable authority and lifecycle evidence for one ranking pass. */
    struct StreamingPriorityEvaluationContext final {
        StreamingPriorityPolicyId policy{};                                       /**< Expected stable policy authority. */
        StreamingPriorityPolicyRevision policyRevision{};                         /**< Expected exact policy publication. */
        WorldPartitionId partition{};                                             /**< Stable partition identity shared by candidates. */
        PartitionEpoch epoch{};                                                   /**< Exact mounted partition incarnation. */
        std::uint64_t serviceTimeMilliseconds{};                                  /**< Current unscaled monotonic service time. */
        StreamingPriorityPolicyState state{StreamingPriorityPolicyState::Closed}; /**< Current lifecycle gate. */
    };

    /** @brief One ranked result with transparent score and bounded queue-age evidence. */
    struct StreamingRankedCellPriority final {
        StreamingCellPriorityCandidate candidate{}; /**< Value-owned candidate; no borrowed input storage. */
        double score{};                             /**< Complete descending priority score. */
        double ageBoost{};                          /**< Clamped additive age term included in score. */
    };

    /**
     * @brief Ranks cell work by distance-weighted priority, bounded age, and stable value ties.
     * @param policy Immutable policy publication.
     * @param context Exact policy, partition incarnation, service time, and lifecycle evidence.
     * @param candidates Caller-owned immutable candidate rows in arbitrary order.
     * @param output Caller-owned storage; the active prefix is written only after every input validates.
     * @return Number of ranked rows or a typed invalid, unsupported, stale, capacity, or lifecycle failure.
     * @details Ranking performs no allocation, blocking, admission, reservation, or ambient mutation. Equal scores use canonical cell
     *          order, then source and value fields, so registration and input iteration order cannot affect distinguishable rows.
     */
    [[nodiscard]] Result<std::size_t> RankStreamingCellPriorities(const StreamingPriorityPolicy &policy,
                                                                  const StreamingPriorityEvaluationContext &context,
                                                                  std::span<const StreamingCellPriorityCandidate> candidates,
                                                                  std::span<StreamingRankedCellPriority> output);
}  // namespace Horo::WorldStreaming
