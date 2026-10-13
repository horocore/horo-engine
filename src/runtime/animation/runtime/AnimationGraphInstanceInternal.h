#pragma once

#include "Horo/Animation/AnimationGraphInstance.h"

#include <array>
#include <thread>
#include <vector>

namespace Horo::Animation::Detail {
    /** @brief Exact player candidate state; integer residual is signed millionths of one time tick. */
    struct GraphPlayerState final {
        AnimationClipCursor cursor{};
        std::int64_t remainder{};
    };

    /** @brief Preallocated transaction buffers and the exact tick/cancellation fence for their publication. */
    struct GraphCandidateState final {
        std::vector<GraphParameterValue> parameters;
        std::vector<GraphPlayerState> players;
        AnimationTickId tick{};
        CancellationToken cancellation{};
    };

    /** @brief Retained immutable curves and their admitted parameter destinations; fixed after preparation. */
    struct GraphCurveBindings final {
        std::vector<std::shared_ptr<const AnimationParameterCurve>> publications;
        std::vector<std::size_t> parameters;
    };

    /** @brief Prepared scalar workspace and exact type-segment offsets. */
    struct GraphScalarWorkspace final {
        std::vector<GraphParameterValue> values;
        std::array<std::size_t, 5> offsets{};
    };

    /** @brief Owner-thread runtime storage; all arrays are fixed during preparation. */
    struct GraphInstanceState final {
        std::thread::id ownerThread{std::this_thread::get_id()};
        AnimationInstanceHandle identity{};
        std::shared_ptr<const AnimationGraphProgram> program;
        std::shared_ptr<const SkeletonAsset> skeleton;
        std::vector<std::shared_ptr<const AnimationClipAsset>> clips;
        GraphCurveBindings curves;
        AnimationClipLimits clipLimits{};
        std::vector<Math::Transform> workingPoses;
        GraphScalarWorkspace scalars;
        std::vector<GraphParameterValue> parameters;
        std::vector<std::uint8_t> queuedTriggers;
        std::array<std::vector<GraphParameterValue>, 2> publishedParameters;
        std::vector<GraphPlayerState> players;
        std::array<std::vector<Math::Transform>, 2> publishedPoses;
        std::array<PoseHandle, 2> publishedHandles{};
        GraphInstancePublication publication{};
        GraphCandidateState candidate;
        CancellationToken ownerCancellation{};
        std::uint64_t nextPoseGeneration{1};
        bool closed{};

        [[nodiscard]] std::span<Math::Transform> Pose(GraphWorkingSlot slot) noexcept {
            const auto joints = program->SkeletonBinding().joints;
            return std::span{workingPoses}.subspan(static_cast<std::size_t>(slot.index) * joints, joints);
        }
    };

    /** @brief Performs only bounded execution in previously admitted storage; failure never publishes state. */
    [[nodiscard]] GraphInstanceOutcome ExecuteGraph(GraphInstanceState &state, const GraphEvaluationInput &input);
}  // namespace Horo::Animation::Detail
