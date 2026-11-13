#pragma once
#include "Horo/Animation/AnimationGraphInstance.h"

#include <limits>
#include <vector>

namespace Horo::Animation::Detail {
    inline constexpr auto NoActionIndex = std::numeric_limits<std::uint32_t>::max();
    struct GraphInstanceState;

    /** @brief One exact regional clip reference and canonical authored successor.
     */
    struct PreparedActionSection final {
        std::uint32_t clip{};
        AnimationClipRegion region{};
        std::uint32_t next{NoActionIndex};
        std::uint32_t loop{NoActionIndex};
        std::vector<std::uint8_t> coverage;
    };

    /** @brief Cold-resolved loop path; section successor remains the exit
     * authority. */
    struct PreparedActionLoop final {
        std::uint32_t first{};
        std::uint32_t last{};
    };

    /** @brief Canonical authored action plus exact immutable clip/mask bindings. */
    struct PreparedAction final {
        std::uint32_t slot{};
        std::uint32_t entry{};
        std::vector<PreparedActionSection> sections;
        std::vector<PreparedActionLoop> loops;
        std::vector<float> mask;
    };

    /** @brief One retained admitted request and its transactionally captured owner
     * intents. */
    struct ActionRecord final {
        GraphActionObservation observation;
        std::uint32_t action{};
        std::uint32_t start{};
        std::uint32_t next{NoActionIndex};
        GraphActionQueuePolicy queue{};
        CancellationToken cancellation;
        GraphActionStopPolicy stop{GraphActionStopPolicy::BlendOut};
        bool cancelIntent{};
        bool releaseIntent{};
    };

    /** @brief One slot's actual cursor is in candidate.players; these values are
     * control/envelope state, not another clock. */
    struct ActionSlotState final {
        std::uint32_t active{NoActionIndex};
        std::uint32_t head{NoActionIndex};
        std::uint32_t tail{NoActionIndex};
        std::uint32_t queued{};
        std::uint32_t interrupt{NoActionIndex};
        std::uint32_t section{};
        std::uint32_t loopPasses{};
        AnimationTime sampleTime{};
        std::int64_t blendElapsed{};
        std::int64_t outElapsed{};
        float weight{};
        float outStart{};
        bool exitLoop{};
        bool directionPending{true};
        GraphActionCompletionReason exitReason{GraphActionCompletionReason::None};
        std::uint32_t handoffAction{NoActionIndex};
        std::uint32_t handoffSection{};
        std::int64_t handoffElapsed{};
        float handoffWeight{};
    };

    /** @brief All request, pose and publication storage is cold-preallocated under
     * the instance budget. */
    struct GraphActionWorkspace final {
        GraphActionLimits limits;
        std::uint64_t nextOrdinal{1};
        std::uint32_t transitions{};
        std::uint32_t boundaryCrossings{};
        std::vector<PreparedAction> definitions;
        std::vector<ActionRecord> records;
        std::vector<ActionRecord> candidateRecords;
        std::vector<ActionSlotState> slots;
        std::vector<ActionSlotState> candidateSlots;
        std::vector<std::uint32_t> admissions;
        std::vector<Math::Transform> overlay;
        std::vector<Math::Transform> committedOverlay;
        std::vector<Math::Transform> frozen;
        std::vector<Math::Transform> committedFrozen;
        std::array<std::vector<GraphActionSlotObservation>, 2> published;
        std::array<std::vector<GraphActionObservation>, 2> completions;
        std::array<std::uint32_t, 2> completionCounts{};
    };

    /** @brief Computes complete cold logical storage/work admission before any
     * action vector allocation. */
    [[nodiscard]] std::uint64_t ActionStorageBytes(const AnimationGraphProgram &, const GraphActionLimits &) noexcept;
    /** @brief Checks finite request/work limits against implementation bounds. */
    [[nodiscard]] bool ValidActionLimits(const GraphActionLimits &) noexcept;
    /** @brief Adds conservative complete action sampling/control cost to existing
     * graph admission. */
    [[nodiscard]] std::uint64_t ActionJointOperations(const GraphInstanceAssets &) noexcept;
    [[nodiscard]] std::uint64_t ActionControlOperations(const AnimationGraphProgram &, const GraphActionLimits &) noexcept;
    /** @brief Prepares immutable exact dependencies and all action
     * runtime/publication storage. */
    [[nodiscard]] GraphInstanceFailure PrepareActions(GraphInstanceState &, const GraphActionLimits &);
    /** @brief Captures and stages all slots once, without allocation or durable
     * command consumption. */
    [[nodiscard]] GraphInstanceOutcome EvaluateActions(GraphInstanceState &, const GraphEvaluationInput &);
    /** @brief Samples the final original per-slot cursors once into their prepared
     * overlays. */
    [[nodiscard]] GraphInstanceOutcome SampleActions(GraphInstanceState &);
    /** @brief Composes a real slot occurrence with its connected base through the
     * existing pose kernel. */
    [[nodiscard]] GraphInstanceOutcome ComposeActionSlot(GraphInstanceState &, const GraphInstructionOccurrence &);
    /** @brief Publishes action facts with the same exact graph pose generation and
     * consumes captured commands. */
    void CommitActions(GraphInstanceState &, std::uint32_t, AnimationTickId) noexcept;
    /** @brief Explicit owner shutdown terminally cancels live records; old
     * publications remain immutable. */
    void ShutdownActions(GraphInstanceState &) noexcept;
    /** @brief Invokes canonical curve policy shared with graph state transitions.
     */
    [[nodiscard]] float ActionCurve(GraphStateBlendCurve, double) noexcept;
    /** @brief Borrows the exact original slot player. */
    [[nodiscard]] std::uint32_t ActionPlayer(const GraphInstanceState &, std::uint32_t) noexcept;
    /** @brief Bounded candidate transition budget; no candidate can silently
     * partially advance. */
    [[nodiscard]] GraphInstanceOutcome SpendActionTransition(GraphInstanceState &) noexcept;
    /** @brief Sets one exact terminal candidate result, never invokes a callback or
     * clip notify. */
    void FinishAction(GraphInstanceState &, std::uint32_t, GraphActionCompletionReason, AnimationTickId) noexcept;
    /** @brief Starts a real admitted request on its declared slot and original
     * cursor. */
    void StartAction(GraphInstanceState &, std::uint32_t, bool) noexcept;
    /** @brief Applies captured FIFO admissions/cancellation to the same candidate
     * record/slot inventory. */
    [[nodiscard]] GraphInstanceOutcome AdmitActionCommands(GraphInstanceState &, AnimationTickId);
    /** @brief Replaces the active request using its exact last-committed overlay
     * and authored handoff. */
    void InterruptAction(GraphInstanceState &, std::uint32_t, AnimationTickId) noexcept;
    /** @brief Begins bounded fade-out or completes an explicit Cut. */
    void StopAction(GraphInstanceState &, std::uint32_t, GraphActionCompletionReason, GraphActionStopPolicy, AnimationTickId) noexcept;
    /** @brief Advances one existing slot's exact directed interval, including queue
     * handoff and sections. */
    [[nodiscard]] GraphInstanceOutcome AdvanceActionSlot(GraphInstanceState &, std::uint32_t, const GraphEvaluationInput &);
}  // namespace Horo::Animation::Detail
