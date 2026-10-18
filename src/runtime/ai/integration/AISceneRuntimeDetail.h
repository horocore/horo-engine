#pragma once

#include "Horo/AI/AISceneActivation.h"

#include <functional>
#include <thread>
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
        /** @brief Original-host copier; no provider or module callback can be installed here. */
        using DiagnosticCapture = std::shared_ptr<const Telemetry::OperationContext> (*)(const Telemetry::OperationContext &);

        /** @brief Binds original-host allocation authority before exposing any capability. */
        AiSceneRuntimeState() noexcept;

        const DiagnosticCapture captureDiagnostics; /**< Immutable original host image allocation authority. */
        const std::thread::id owner{std::this_thread::get_id()};
        AiSceneActivationBinding binding;
        std::vector<AgentRuntimeState> agents;
        std::unordered_multimap<Runtime::EntityRef, std::size_t, EntityRefHash> agentsByOwner;
        std::uint32_t nextTaskSlot{};
        std::uint64_t revision{1};
        std::uint64_t lastAgentIdentity{}; /**< Owner-issued prefab identity high-water; advanced only with aggregate publication. */
        std::unique_ptr<AiTaskScheduler> scheduler;
        bool closed{};
    };

    /** @brief Resolves an exact live agent generation. @param state Owning publication. @param handle Requested handle. @return Slot or
     * null. */
    [[nodiscard]] AgentRuntimeState *FindAgent(AiSceneRuntimeState &state, AgentHandle handle) noexcept;
    /** @brief Admits one task through the scene-owned lifecycle and mutation fence. */
    [[nodiscard]] Result<TaskHandle> StartOwnedTask(AiSceneRuntimeState &state, AgentHandle agent, TaskId task,
                                                    CancellationToken capabilityCancellation = {},
                                                    const Telemetry::OperationContext *diagnostics = nullptr);
    /** @brief Cancels tasks and invalidates blackboard leases before releasing transient resources. @param agent Owned slot. */
    void CancelOwnedWork(AgentRuntimeState &agent) noexcept;
    /** @brief Tears down every owned agent. @param state Detached or active population. */
    void ShutdownStateContents(AiSceneRuntimeState &state) noexcept;

    /** @brief Builds one detached structural adapter against a retained owner publication. */
    [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(
        AiSceneRuntime &runtime, std::span<const AiControllerDescriptor> descriptors);

    /** @brief Validates and constructs detached agents with exact destination slots and generations. */
    [[nodiscard]] Result<std::vector<AgentRuntimeState>> PrepareStructuralAgents(AiSceneActivationBinding binding,
                                                                                 std::span<const AiSceneAgentDescriptor> agents,
                                                                                 std::span<const AiControllerDescriptor> descriptors,
                                                                                 AiCapabilitySet capabilities,
                                                                                 std::span<const Horo::Handle<AgentHandleTag>> slots);
}  // namespace Horo::AI::Detail
