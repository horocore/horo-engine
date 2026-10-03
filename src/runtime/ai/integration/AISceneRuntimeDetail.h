#pragma once

#include "Horo/AI/AISceneActivation.h"

#include <functional>
#include <unordered_map>

namespace Horo::AI::Detail {
    /** @brief Scene-owner storage; transient work is deliberately outside canonical snapshots. */
    struct AgentRuntimeState final {
        AiAgentRuntimeRecord record;
        std::shared_ptr<const BlackboardSchema> schema;
        BlackboardInstanceBinding blackboardBinding;
        std::unique_ptr<BlackboardInstance> blackboard;
        std::unique_ptr<AiTaskLifecycle> task;
        CancellationSource cancellation;
        bool retired{};
    };

    /** @brief Exact Scene/entity generation hash for the owner index. */
    struct EntityRefHash final {
        [[nodiscard]] std::size_t operator()(const Runtime::EntityRef &entity) const noexcept;
    };

    /** @brief One published AI population and its owner-thread mutation fence. */
    struct AiSceneRuntimeState final {
        AiSceneActivationBinding binding;
        std::vector<AgentRuntimeState> agents;
        std::unordered_multimap<Runtime::EntityRef, std::size_t, EntityRefHash> agentsByOwner;
        std::uint32_t nextTaskSlot{};
        std::uint64_t revision{1};
    };

    /** @brief Resolves an exact live agent generation. @param state Owning publication. @param handle Requested handle. @return Slot or
     * null. */
    [[nodiscard]] AgentRuntimeState *FindAgent(AiSceneRuntimeState &state, AgentHandle handle) noexcept;
    /** @brief Cancels tasks and invalidates blackboard leases before releasing transient resources. @param agent Owned slot. */
    void CancelOwnedWork(AgentRuntimeState &agent) noexcept;
    /** @brief Tears down every owned agent. @param state Detached or active population. */
    void ShutdownStateContents(AiSceneRuntimeState &state) noexcept;
}  // namespace Horo::AI::Detail
