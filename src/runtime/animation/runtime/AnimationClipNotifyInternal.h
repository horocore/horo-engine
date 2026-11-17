#pragma once

#include "Horo/Animation/AnimationGraphInstance.h"

#include <array>
#include <vector>

namespace Horo::Animation::Detail {
    struct GraphInstanceState;

    /** @brief Candidate-only event identity and duration ownership; never a playback clock. */
    struct NotifyPlayerState final {
        std::uint64_t serial{};
        std::int64_t loopOrdinal{};
        std::uint32_t clip{};
        AnimationClipRegion region{};
        AnimationTime sample{};
        GraphActionHandle action{};
        AnimationNotifyReason originReason{AnimationNotifyReason::Activation};
        bool initialized{};
        bool visited{};
        bool travelled{};
    };

    /** @brief One cold subscribed owner queue, retaining values rather than clip pointers. */
    struct NotifyConsumerQueue final {
        AnimationEventConsumerId consumer{};
        std::vector<AnimationEventTypeBinding> types;
        std::vector<AnimationEventOccurrence> values;
        std::uint32_t head{};
        std::uint32_t count{};
    };

    /** @brief One directed boundary candidate sorted in caller-owned prepared scratch. */
    struct NotifyHit final {
        std::uint32_t marker{};
        AnimationTime time{};
        AnimationNotifyPhase phase{};
        AnimationNotifyReason reason{AnimationNotifyReason::Crossed};
    };

    /** @brief Cold allocated transaction, duration and queue storage, all fixed during ticks. */
    struct NotifyWorkspace final {
        GraphClipEventSettings settings{};
        AnimationTickId tick{};
        GraphInstanceFailure failure{GraphInstanceFailure::None};
        std::uint32_t markersPerPlayer{};
        std::uint32_t count{};
        std::uint32_t hitCount{};
        std::vector<NotifyPlayerState> players;
        std::vector<NotifyPlayerState> candidate;
        std::vector<std::uint64_t> windows;
        std::vector<std::uint64_t> candidateWindows;
        std::vector<AnimationEventOccurrence> occurrences;
        std::array<std::vector<AnimationEventOccurrence>, 2> published;
        std::array<std::uint32_t, 2> publishedCounts{};
        std::vector<AnimationClipTraversalSegment> trace;
        std::vector<NotifyHit> hits;
        std::vector<NotifyConsumerQueue> consumers;
    };

    /** @brief Admits immutable schemas, real domain, subscription capacities and complete work before allocation. */
    [[nodiscard]] GraphInstanceFailure AdmitClipNotifies(const GraphInstanceAssets &, const GraphInstanceCreateContext &) noexcept;
    /** @brief Charges every retained logical fixed payload, queue, window and double publication element. */
    [[nodiscard]] std::uint64_t ClipNotifyStorageBytes(const GraphInstanceAssets &, const GraphInstanceCreateContext &) noexcept;
    /** @brief Bounds complete directed scans, sorting and subscribed queue matching under admitted player limits. */
    [[nodiscard]] std::uint64_t ClipNotifyOperations(const GraphInstanceAssets &, const GraphInstanceCreateContext &) noexcept;
    /** @brief Copies all cold subscriptions and allocates fixed candidate/publication/queue workspaces. */
    [[nodiscard]] GraphInstanceFailure PrepareClipNotifies(GraphInstanceState &, const GraphInstanceCreateContext &);
    /** @brief Starts candidate notify metadata from the same committed graph tick state without allocating. */
    void BeginClipNotifies(GraphInstanceState &, AnimationTickId) noexcept;
    /** @brief Closes inactive candidate windows and admits every subscribed queue before Commit. */
    [[nodiscard]] GraphInstanceOutcome CompleteClipNotifies(GraphInstanceState &) noexcept;
    /** @brief Publishes candidate values and duration ownership at the existing graph Commit fence. */
    void CommitClipNotifies(GraphInstanceState &, std::uint32_t) noexcept;
    /** @brief Tests actual unread subscribed values before transactional replacement. */
    [[nodiscard]] bool ClipNotifyBacklog(const GraphInstanceState &) noexcept;
    /** @brief Consumes one exact registered owner queue prefix without allocation or callbacks. */
    [[nodiscard]] GraphInstanceOutcome DrainClipNotifyQueue(GraphInstanceState &, AnimationEventConsumerId,
                                                            std::span<AnimationEventOccurrence>, std::uint32_t &) noexcept;
    /** @brief Uses actual staged state-machine transition contribution and authored event policy. */
    [[nodiscard]] bool ClipNotifyEligible(const GraphInstanceState &, const GraphInstructionOccurrence &) noexcept;
    /** @brief Closes only duration ownership held by this exact active action request. */
    void CloseActionClipNotifies(GraphInstanceState &, std::uint32_t, GraphActionCompletionReason) noexcept;
    /** @brief Retires candidate duration ownership before actual player reuse, retaining nonwrapping serial identity. */
    void ResetClipNotifyPlayer(GraphInstanceState &, std::uint32_t, AnimationNotifyReason,
                               AnimationTraversalDirection = AnimationTraversalDirection::Forward) noexcept;
    /** @brief Publishes only the actual authored action-loop edge already crossed by the canonical action controller. */
    void EnterActionClipNotifyLoop(GraphInstanceState &, std::uint32_t, AnimationTraversalDirection) noexcept;
    /** @brief Closes delivered durations at the actual held sample with a truthful lifecycle reason. */
    void CloseClipNotifyPlayer(GraphInstanceState &, std::uint32_t, AnimationNotifyReason) noexcept;
    /** @brief Visits only the real canonical directed region path and retains owned candidate payload values. */
    [[nodiscard]] GraphInstanceOutcome StageClipNotifies(GraphInstanceState &, std::uint32_t, std::uint32_t, AnimationClipCursor,
                                                         AnimationTimeDelta, AnimationClipRegion, bool, GraphActionHandle = {}) noexcept;
    /** @brief Stages one bounded typed occurrence and corresponding candidate duration serial. */
    void EmitClipNotify(GraphInstanceState &, std::uint32_t, const NotifyHit &, AnimationTraversalDirection) noexcept;
    /** @brief Orders real directed point and duration crossings in preallocated scratch. */
    void ClipNotifySegment(GraphInstanceState &, std::uint32_t, const AnimationClipTraversalSegment &, bool) noexcept;
    /** @brief Reconciles exact directed origin occupancy without synthesizing an interval. */
    void ClipNotifyOrigin(GraphInstanceState &, std::uint32_t, AnimationTime, AnimationTraversalDirection, bool, AnimationNotifyReason,
                          bool) noexcept;
}  // namespace Horo::Animation::Detail
