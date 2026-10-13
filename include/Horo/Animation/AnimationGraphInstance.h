#pragma once

/** @file AnimationGraphInstance.h
 * @brief Explicitly prepared, allocation-free transactional animation graph runtime.
 */

#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Animation/AnimationParameterCurve.h"
#include "Horo/Foundation/CancellationToken.h"

#include <memory>
#include <span>

namespace Horo::Animation {
    /** @brief Signed player-local millionths (1,000,000 is normal speed); bounded to +/-16x. */
    struct GraphPlaybackRate final {
        std::int32_t millionths{1'000'000};
        auto operator<=>(const GraphPlaybackRate &) const = default;
    };

    /** @brief Exact typed parameter snapshot entry, identified independently of declaration order. */
    struct GraphParameterInput final {
        GraphParameterId id{};
        GraphParameterValue value{0.0F};
    };

    /** @brief Immutable snapshots retained by an instance; no raw dependency borrow survives preparation. */
    struct GraphInstanceAssets final {
        std::shared_ptr<const AnimationGraphProgram> program;
        std::shared_ptr<const SkeletonAsset> skeleton;
        SkeletonAssetGeneration skeletonGeneration{}; /**< Exact generation issued with this pinned skeleton publication. */
        std::span<const std::shared_ptr<const AnimationClipAsset>> clips;
        std::span<const std::shared_ptr<const AnimationParameterCurve>> curves{}; /**< Unique float bindings; pinned through reload. */
    };

    /** @brief Owner-assigned identity and finite storage/work admission, captured at the control boundary. */
    struct GraphInstanceCreateContext final {
        AnimationInstanceHandle instance{};
        std::uint64_t maximumStorageBytes{256ULL * 1024ULL * 1024ULL};
        std::uint32_t maximumInstructions{AnimationGraphHardLimits::EvaluationInstructions};
        std::uint64_t maximumJointOperations{64ULL * 1024ULL * 1024ULL}; /**< Per-tick joint writes and bounded key-search visits. */
        AnimationClipLimits clipLimits{};
        CancellationToken cancellation{};
        std::uint64_t maximumControlOperations{8ULL * 1024ULL *
                                               1024ULL}; /**< Whole-plan guard/reset/condition/event work before allocation. */
    };

    /** @brief One attempted fixed tick; caller views are copied before execution and never retained. */
    struct GraphEvaluationInput final {
        AnimationTickId tick{};
        AnimationTimeDelta delta{};
        GraphPlaybackRate rate{};
        std::span<const GraphParameterInput> parameters{}; /**< Defaults, curves, explicit scalars; triggers OR queued events. */
        CancellationToken cancellation{};
        AnimationTime curveTime{}; /**< Exact authored curve time; independent of each clip's local playback cursor. */
    };

    /** @brief Committed immutable pose identities; failed attempts never change these values. */
    struct GraphInstancePublication final {
        AnimationTickId tick{};
        PoseHandle previousPose{};
        PoseHandle currentPose{};
    };
    /** @brief Committed state lifecycle records; clip notifies remain their separate Animation responsibility. */
    enum class GraphStateEventKind : std::uint8_t {
        Entered,
        Exited,
        TransitionStarted,
        TransitionInterrupted,
        TransitionCompleted,
        Loop
    };

    /** @brief Copied immutable lifecycle occurrence, fenced by instance, compiled occurrence, tick and entry generation. */
    struct GraphStateEvent final {
        AnimationInstanceHandle instance{};
        AnimationTickId tick{};
        GraphSourceLocation source{};
        std::uint32_t occurrence{};
        std::uint32_t ordinal{};
        GraphStateEventKind kind{};
        GraphStateId state{};
        GraphTransitionId transition{};
        std::uint64_t activation{};
        std::uint64_t loopOrdinal{};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        bool operator==(const GraphStateEvent &) const = default;
    };

    /** @brief Allocation-free graph runtime failure vocabulary, independent of diagnostic string ownership. */
    enum class GraphInstanceFailure : std::uint8_t {
        None,
        AdmissionRejected,
        Cancelled,
        Malformed,
        TypeMismatch,
        LimitExceeded,
        BindingMismatch,
        HandleMalformed,
        HandleStale,
        ThreadViolation,
        TickStale,
        StateInvalid,
        ClipFailure
    };

    /** @brief Exact bounded hot-path outcome; successful commit also carries publication evidence. */
    struct GraphInstanceOutcome final {
        GraphInstanceFailure failure{GraphInstanceFailure::None};
        AnimationClipFailureInfo clipFailure{};
        GraphInstancePublication publication{};

        [[nodiscard]] bool HasValue() const noexcept {
            return failure == GraphInstanceFailure::None;
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }
    };

    /** @brief Projects a typed runtime failure into an owning diagnostic outside frame-hot execution.
     * @param outcome Failed typed operation outcome.
     * @return Exact registered diagnostic; may allocate.
     * @pre outcome.HasError().
     */
    [[nodiscard]] Error DescribeGraphInstanceFailure(const GraphInstanceOutcome &outcome);

    namespace Detail {
        struct GraphInstanceState;
    }

    /**
     * @brief Animation-owned graph execution and publication for one exact runtime incarnation.
     * @details The host creates/replaces this owner at a control boundary, supplies fixed ticks,
     * invokes Evaluate, and calls Commit only after the complete tick succeeds (otherwise Abort).
     * All operations, destruction and moves belong to the creating thread. Assets are retained
     * immutable snapshots; reload requires explicit replacement with a fresh instance generation.
     * Consumers copy committed local poses into their own storage; no borrowed pose pointer or
     * implicit lease escapes. The last two committed poses remain readable until recycled by a
     * later successful commit. Shutdown closes execution but preserves existing publications.
     * Evaluate/Commit/Abort/CopyPose/Shutdown allocate nothing and perform no blocking I/O.
     */
    class AnimationGraphInstance final {
    public:
        /** @brief Prepares all runtime storage and pins exact compiled dependencies.
         * @param assets Immutable program, skeleton and exact clip snapshots.
         * @param context Host-assigned identity, cancellation and finite admission limits.
         * @return Complete prepared owner or typed admission/binding/capacity failure.
         * @pre Load/control boundary; never call from frame-hot execution.
         */
        [[nodiscard]] static Result<AnimationGraphInstance> Create(const GraphInstanceAssets &assets,
                                                                   const GraphInstanceCreateContext &context);
        /** @brief Prepares and atomically installs an explicit dependency reload at a control boundary.
         * @param assets Exact replacement immutable snapshots.
         * @param context Same owner/component/slot and next non-wrapping instance generation.
         * @return Success retires old pose handles; failure retains all old state and publications.
         * @pre Owner thread, no pending tick; may allocate, never call from frame-hot evaluation.
         */
        [[nodiscard]] Result<void> Replace(const GraphInstanceAssets &assets, const GraphInstanceCreateContext &context);
        AnimationGraphInstance(const AnimationGraphInstance &) = delete;
        AnimationGraphInstance &operator=(const AnimationGraphInstance &) = delete;
        AnimationGraphInstance(AnimationGraphInstance &&) noexcept;
        AnimationGraphInstance &operator=(AnimationGraphInstance &&) noexcept;
        ~AnimationGraphInstance();

        /** @brief Stages one complete evaluation without publishing cursors, parameters or poses.
         * @param input Attempted fixed tick and immutable typed inputs.
         * @return Success leaves a pending candidate; failure leaves committed state unchanged.
         * @pre No candidate is already pending; tick strictly exceeds the last committed tick.
         */
        [[nodiscard]] GraphInstanceOutcome Evaluate(const GraphEvaluationInput &input);
        /** @brief Atomically publishes a successfully staged tick after owner tick preflight.
         * @param tick Exact pending tick.
         * @return Complete publication or cancellation/lifecycle/tick failure with no commit.
         */
        [[nodiscard]] GraphInstanceOutcome Commit(AnimationTickId tick);
        /** @brief Discards a pending candidate; committed state is unchanged. */
        [[nodiscard]] GraphInstanceOutcome Abort();
        /** @brief Queues one coalesced one-tick trigger without allocation.
         * @param parameter Stable declared trigger identity; scalar and missing identities are rejected.
         * @return Success or typed owner, lifecycle, identity or type diagnostic.
         * @details Successful Commit consumes queued triggers. Abort, failed evaluation and cancelled Commit retain them.
         * Queueing while a candidate is pending is rejected. Explicit true input triggers apply only to that attempted tick.
         */
        [[nodiscard]] GraphInstanceOutcome QueueTrigger(GraphParameterId parameter);
        /** @brief Copies an immutable typed gameplay parameter block from an exact committed pose publication.
         * @param publication Exact resident instance/slot/generation identity returned by Commit.
         * @param output Complete caller-owned parameter storage; results are ordered by stable parameter identity.
         * @return Success or typed owner, malformed/stale publication or size diagnostic before any writes.
         * @details Both resident publications remain queryable, including after Shutdown. Trigger values describe that
         * publication only; the next successful tick clears them unless another event is queued or explicitly supplied.
         */
        [[nodiscard]] GraphInstanceOutcome CopyParameters(const PoseHandle &publication, std::span<GraphParameterInput> output) const;
        /** @brief Copies the complete state lifecycle event block for an exact resident committed publication.
         * @param publication Exact pose publication returned by Commit; both retained publications are readable after Shutdown.
         * @param output Caller-owned capacity for the complete event block; larger spans are permitted.
         * @param written Receives copied count on success; unchanged together with output on failure.
         * @return Typed owner, stale/size/lifecycle outcome before any partial write. No allocation or callback occurs.
         */
        [[nodiscard]] GraphInstanceOutcome CopyStateEvents(const PoseHandle &publication, std::span<GraphStateEvent> output,
                                                           std::uint32_t &written) const;
        /** @brief Copies an exact resident immutable committed local pose.
         * @param pose Exact instance, recyclable slot and semantic generation.
         * @param output Caller-owned complete skeleton pose storage.
         * @return Success or typed stale/binding/size failure before any writes.
         */
        [[nodiscard]] GraphInstanceOutcome CopyPose(const PoseHandle &pose, std::span<Math::Transform> output) const;
        /** @brief Returns last committed metadata. @return Empty handles before the first commit. */
        [[nodiscard]] GraphInstancePublication Publication() const noexcept;
        /** @brief Closes execution and discards candidates, retaining committed copied-query storage.
         * @return Success or owner-thread failure; idempotent.
         */
        [[nodiscard]] GraphInstanceOutcome Shutdown();

    private:
        explicit AnimationGraphInstance(std::unique_ptr<Detail::GraphInstanceState> state) noexcept;
        std::unique_ptr<Detail::GraphInstanceState> state_;
    };
}  // namespace Horo::Animation
