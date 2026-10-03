#pragma once

#include "Horo/AI/BehaviorTreeRuntime.h"

namespace Horo::AI {
    /** @brief Owner-only state for one admitted control node. */
    struct BehaviorTreeInstance::NodeState final {
        AiTaskState status{AiTaskState::Idle};
        std::size_t cursor{};
        std::uint32_t iterations{};
        std::uint64_t startedTick{};
        std::uint64_t completedTick{};
        bool cooldownSet{};
        std::uint64_t checkedEvaluation{};
        bool condition{};
        std::optional<AiTaskLifecycle> task;
        AiTaskContinuation continuation;
    };

    /** @brief Owner-only service clock and blackboard revision fence. */
    struct BehaviorTreeInstance::ServiceState final {
        std::uint64_t tick{};
        std::uint64_t revision{};
        bool active{};
    };

    /** @brief Bounded iterative traversal frame. */
    struct BehaviorTreeInstance::Frame final {
        std::size_t node{};
        std::size_t nextChild{};
        bool entered{};
    };

}  // namespace Horo::AI
