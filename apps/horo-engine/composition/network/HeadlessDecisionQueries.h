#pragma once
/** @file HeadlessDecisionQueries.h @brief Exact committed query execution for a headless Scene owner. */

#include "HeadlessCookedDecisionAdmission.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/AI/DecisionQueryTask.h"

namespace Horo::Application::Internal {
    /** @brief Owns the query binding and fixed-step phases for one authoritative runtime world. */
    class HeadlessDecisionQueries final : public AI::IDecisionQueryTaskBinder, private AI::IDecisionQueryTaskHost {
    public:
        /** @brief Freezes the matching query plans and allocates bounded phase state before Scene activation. */
        [[nodiscard]] static Result<std::shared_ptr<HeadlessDecisionQueries>> Create(
            HeadlessQueryPlanCatalog plans, std::shared_ptr<const AI::QuerySchemaRegistry> registry,
            const AI::AiTaskSchedulerSettings &settings);

        /** @copydoc AI::IDecisionQueryTaskBinder::BindQueryTask */
        [[nodiscard]] Result<std::unique_ptr<AI::IBehaviorTreeExecutor>> BindQueryTask(
            const AI::BehaviorTreeExecutionPlan &tree, const AI::DecisionNodeConfiguration &configuration) override;

        /** @brief Publishes eligible results, evaluates decisions, and advances admitted queries on the same fixed tick. */
        [[nodiscard]] Result<void> RunAtFixedTick(AI::AiSceneRuntime &runtime, const Runtime::RuntimeSceneView &scene,
                                                  Math::WorldCoordinate64 origin, std::uint64_t tick);

    private:
        struct TickScope final {
            AI::AiSceneRuntime &runtime;
            const Runtime::RuntimeSceneView &scene;
            Math::WorldCoordinate64 origin;
            std::uint64_t tick;
            std::size_t active{};
        };

        struct AgentBudget final {
            std::uint64_t tick{};
            std::size_t remaining{};
        };

        /** @brief Captures the current Scene's exact query context and reserves one live execution. */
        [[nodiscard]] Result<std::unique_ptr<AI::QueryExecution>> Admit(const AI::DecisionQueryTaskDescriptor &descriptor,
                                                                        const AI::AiTaskOperationContext &operation,
                                                                        std::uint64_t submitted, std::uint64_t deadline) override;
        /** @brief Publishes at most one result per active query through the Scene-owned BlackboardSync lane. */
        [[nodiscard]] Result<void> PublishOutputs(TickScope &scope);
        /** @brief Issues bounded allowances in round-robin occurrence order without allocating. */
        void AdvanceQueries(std::uint64_t tick);

        HeadlessQueryPlanCatalog plans_;
        std::shared_ptr<const AI::QuerySchemaRegistry> registry_;
        std::optional<AI::QueryContextProviderRegistry> contexts_;
        std::vector<std::weak_ptr<AI::DecisionQueryTask>> tasks_;
        std::vector<AgentBudget> budgets_;
        std::size_t maximumActive_{};
        std::size_t cursor_{};
        TickScope *scope_{};
    };
}  // namespace Horo::Application::Internal
