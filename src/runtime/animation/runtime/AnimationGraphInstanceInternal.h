#pragma once

#include "AnimationActionInternal.h"
#include "AnimationStateMachineInternal.h"
#include "Horo/Animation/AnimationGraphInstance.h"

#include <array>
#include <thread>
#include <vector>

namespace Horo::Animation::Detail {
    /** @brief Exact player candidate state; integer residual is signed millionths
     * of one time tick. */
    struct GraphPlayerState final {
        AnimationClipCursor cursor{};
        std::int64_t remainder{};
    };

    /** @brief Exact per-entry authored clock and residual; no clip cursor is
     * duplicated here. */
    struct GraphStateClock final {
        GraphPlayerState rate{};
        AnimationTime elapsed{};
        std::uint64_t loopOrdinal{};
    };

    /** @brief One complete committed/candidate machine incarnation, with no owning
     * views or heap work. */
    struct GraphMachineState final {
        StateMachineRuntime runtime{};
        std::array<GraphStateClock, 2> clocks{};
        std::uint8_t sourceLane{};
        std::uint8_t destinationLane{1};
        std::array<bool, 2> held{};
        StateMachineStepOutcome outcome{};
        bool visited{};
        bool retiring{};
    };

    /** @brief Prepared immutable control tables for one exact compiled machine
     * occurrence. */
    struct GraphPreparedMachine final {
        std::vector<StateMachineState> states;
        std::vector<StateMachineTransition> transitions;
        std::vector<StateMachineCondition> conditions;
        std::uint32_t initial{};

        [[nodiscard]] StateMachinePlan Plan() const noexcept {
            return {states, transitions, conditions, initial};
        }
    };

    /** @brief Preallocated transaction buffers and the exact tick/cancellation
     * fence for their publication. */
    struct GraphCandidateState final {
        std::vector<GraphParameterValue> parameters;
        std::vector<GraphPlayerState> players;
        std::vector<GraphMachineState> machines;
        std::vector<std::uint8_t> latches;
        std::vector<GraphStateEvent> events;
        std::uint32_t eventCount{};
        AnimationTickId tick{};
        CancellationToken cancellation{};
    };

    /** @brief Retained immutable curves and their admitted parameter destinations;
     * fixed after preparation. */
    struct GraphCurveBindings final {
        std::vector<std::shared_ptr<const AnimationParameterCurve>> publications;
        std::vector<std::size_t> parameters;
    };

    /** @brief Prepared scalar workspace and exact type-segment offsets. */
    struct GraphScalarWorkspace final {
        std::vector<GraphParameterValue> values;
        std::array<std::size_t, 5> offsets{};
    };

    /** @brief Equal-sized prepared pose values and authored-contribution coverage.
     */
    struct GraphPoseWorkspace final {
        std::vector<Math::Transform> transforms;
        std::vector<std::uint8_t> coverage;
    };

    /** @brief Complete double-buffered owner publication, with its resident handles
     * and nonwrapping generation fence. */
    struct GraphPublicationStorage final {
        std::array<std::vector<GraphParameterValue>, 2> parameters;
        std::array<std::vector<GraphStateEvent>, 2> events;
        std::array<std::uint32_t, 2> eventCounts{};
        std::array<std::vector<Math::Transform>, 2> poses;
        std::array<PoseHandle, 2> handles{};
        GraphInstancePublication publication{};
        std::uint64_t nextGeneration{1};
    };

    /** @brief Prepared tree member and invocation-local traversal; its sole cursor
     * remains GraphPlayerState. */
    struct GraphBlendMemberWork final {
        std::uint32_t tree{};
        std::uint32_t sample{};
        double leaderWeight{};
        GraphBlendMemberObservation observation{};
        AnimationClipTraversal traversal{};
        AnimationTimeDelta delta{};
    };

    /** @brief One occurrence membership and branch-local input; no independent
     * playback clock. */
    struct GraphBlendTreeWork final {
        std::uint32_t occurrence{};
        std::uint32_t group{};
        AnimationTimeDelta delta{};
        GraphPlaybackRate rate{};
        bool active{};
        bool frozenSource{};
    };

    /** @brief Bounded immutable membership with one selected leader per candidate
     * evaluation. */
    struct GraphSyncGroupWork final {
        GraphSyncGroupId id{};
        GraphSyncPolicy policy{};
        std::vector<std::uint32_t> members;
    };

    /** @brief All sync scratch and double-buffered observations are allocated at
     * instance admission. */
    struct GraphBlendWorkspace final {
        std::vector<GraphBlendTreeWork> trees;
        std::vector<GraphBlendMemberWork> members;
        std::vector<GraphSyncGroupWork> groups;
        std::array<std::vector<GraphBlendMemberObservation>, 2> published;
    };

    /** @brief Owner-thread runtime storage; all arrays are fixed during
     * preparation. */
    struct GraphInstanceState final {
        std::thread::id ownerThread{std::this_thread::get_id()};
        AnimationInstanceHandle identity{};
        std::shared_ptr<const AnimationGraphProgram> program;
        std::shared_ptr<const SkeletonAsset> skeleton;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        GraphCurveBindings curves;
        GraphBlendWorkspace blends;
        GraphActionWorkspace actions;
        AnimationClipLimits clipLimits{};
        GraphPoseWorkspace poses;
        GraphScalarWorkspace scalars;
        std::vector<GraphParameterValue> parameters;
        std::vector<std::uint8_t> queuedTriggers;
        std::vector<GraphPlayerState> players;
        std::vector<GraphPreparedMachine> preparedMachines;
        std::vector<std::uint8_t> latchScratch;
        std::vector<GraphMachineState> machines;
        std::vector<std::uint8_t> latches;
        GraphCandidateState candidate;
        GraphPublicationStorage published;
        CancellationToken ownerCancellation{};
        bool closed{};

        [[nodiscard]] std::span<Math::Transform> Pose(GraphWorkingSlot slot) noexcept {
            const auto joints = program->SkeletonBinding().joints;
            return std::span{poses.transforms}.subspan(static_cast<std::size_t>(slot.index) * joints, joints);
        }

        /** @brief Returns authored-contribution coverage for the same exact pose
         * writer slot. */
        [[nodiscard]] std::span<std::uint8_t> Coverage(GraphWorkingSlot slot) noexcept {
            const auto joints = program->SkeletonBinding().joints;
            return std::span{poses.coverage}.subspan(static_cast<std::size_t>(slot.index) * joints, joints);
        }
    };

    /** @brief Cold membership preparation and exact group work/storage admission.
     */
    [[nodiscard]] GraphInstanceFailure PrepareGraphBlendTrees(GraphInstanceState &state);
    /** @brief Computes admitted geometry weights without allocation or cursor
     * mutation. */
    [[nodiscard]] GraphInstanceOutcome ComputeGraphBlendWeights(const GraphBlendTreeNode &tree, float x, float y,
                                                                std::span<float> weights) noexcept;
    /** @brief Plans active groups after state controls have staged their guards,
     * before any pose is sampled. */
    [[nodiscard]] GraphInstanceOutcome PrepareGraphSync(GraphInstanceState &state, const GraphEvaluationInput &input);
    /** @brief Samples the already planned original players and composes normalized
     * member poses. */
    [[nodiscard]] GraphInstanceOutcome ExecuteGraphBlendTree(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence);

    /** @brief Performs only bounded execution in previously admitted storage;
     * failure never publishes state. */
    [[nodiscard]] GraphInstanceOutcome ExecuteGraph(GraphInstanceState &state, const GraphEvaluationInput &input);
    /** @brief Shared exact rate scaling for clip players and state clocks,
     * retaining their separate admitted residuals. */
    [[nodiscard]] AnimationClipOutcome<AnimationTimeDelta> ScaleGraphDelta(const GraphEvaluationInput &input,
                                                                           GraphPlayerState &player) noexcept;
    /** @brief Resolves compiled branch guards and supplies entry-zero delta for
     * newly activated destination branches. */
    [[nodiscard]] bool StateBranchInput(const GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                        GraphEvaluationInput &input) noexcept;
    /** @brief Checks a lane incarnation; retirement preflight may inspect the cold
     * initial lane before control admission. */
    [[nodiscard]] bool GraphStateGuardActive(const GraphInstanceState &state, const GraphStateGuard &guard, bool allowCold) noexcept;
    /** @brief Resets candidate control/latches while preserving its monotonic
     * next-entry identity. */
    void ResetGraphStateControl(GraphInstanceState &state, std::uint32_t machine) noexcept;
    /** @brief Stages control state and events before the compiler's guarded child
     * occurrences. */
    [[nodiscard]] GraphInstanceOutcome BeginGraphStateMachine(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                              const GraphEvaluationInput &input);
    /** @brief Composes the compiled machine output through the existing shared pose
     * composition seam. */
    [[nodiscard]] GraphInstanceOutcome EndGraphStateMachine(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence);
    /** @brief Allocates and resolves immutable machine control tables only at
     * instance preparation. */
    [[nodiscard]] GraphInstanceFailure PrepareGraphStateMachines(GraphInstanceState &state);
    /** @brief Borrows the exact immutable occurrence interval of one
     * compiler-admitted state lane.
     * @param state Owner retaining the immutable prepared program for the
     * invocation.
     * @param branch Previously admitted branch ranges in that program.
     * @param lane Previously admitted source or destination lane (0 or 1).
     * @return Non-owning ordered span; it is never stored beyond the invocation and
     * performs no allocation.
     */
    [[nodiscard]] std::span<const GraphInstructionOccurrence> GraphStateBranchOccurrences(const GraphInstanceState &state,
                                                                                          const GraphStateBranch &branch,
                                                                                          std::uint8_t lane) noexcept;
    /** @brief Preflights the exact shared clip traversals for a complete active
     * state branch, optionally staging retired cursors. */
    [[nodiscard]] GraphInstanceOutcome TraverseGraphStateBranch(GraphInstanceState &state, std::uint32_t machine,
                                                                const GraphStateBranch &branch, std::uint8_t lane,
                                                                const GraphEvaluationInput &input, bool stageCursors);
    /** @brief Retires active nested control owners with complete staged
     * exit/interruption records, before branch storage reuse. */
    [[nodiscard]] GraphInstanceOutcome RetireGraphStateBranch(GraphInstanceState &state, std::uint32_t machine,
                                                              const GraphStateBranch &branch, std::uint8_t lane,
                                                              const GraphEvaluationInput &input);
    /** @brief Appends bounded private lifecycle records to the actual instance tick
     * candidate. */
    [[nodiscard]] GraphInstanceOutcome AppendGraphStateEvents(GraphInstanceState &state, const GraphInstructionOccurrence &occurrence,
                                                              const GraphEvaluationInput &input, std::span<const StateMachineEvent> events);
}  // namespace Horo::Animation::Detail
