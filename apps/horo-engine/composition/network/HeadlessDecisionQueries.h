#pragma once
/** @file HeadlessDecisionQueries.h @brief Exact committed query execution for a headless Scene owner. */

#include "HeadlessCookedDecisionAdmission.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/AI/EnvironmentQueryNavigation.h"
#include "Horo/AI/EnvironmentQueryPhysics.h"

namespace Horo::Physics {
    class PhysicsWorld;
}

namespace Horo::Application::Internal {
    class ScenePhysicsHost;
    class HeadlessNavigationPathScope;

    /** @brief Owns the query binding and fixed-step phases for one authoritative runtime world. */
    class HeadlessDecisionQueries final : public AI::IDecisionQueryTaskBinder,
                                          private AI::IDecisionQueryTaskHost,
                                          private AI::IQueryPhysicsVisibilityOwner,
                                          private AI::IQueryNavigationOwner {
    public:
        /** @brief Freezes the matching query plans and allocates bounded phase state before Scene activation. */
        [[nodiscard]] static Result<std::shared_ptr<HeadlessDecisionQueries>> Create(
            HeadlessQueryPlanCatalog plans, std::shared_ptr<const AI::QuerySchemaRegistry> registry,
            const AI::AiTaskSchedulerSettings &settings, std::size_t workUnitsLimit,
            std::optional<AI::QueryResultCacheLimits> cacheLimits = std::nullopt);

        /** @copydoc AI::IDecisionQueryTaskBinder::BindQueryTask */
        [[nodiscard]] Result<std::unique_ptr<AI::IBehaviorTreeExecutor>> BindQueryTask(
            const AI::BehaviorTreeExecutionPlan &tree, const AI::DecisionNodeConfiguration &configuration) override;

        /** @brief Publishes eligible results, evaluates decisions, and advances admitted queries on the same fixed tick. */
        [[nodiscard]] Result<void> RunAtFixedTick(AI::AiSceneRuntime &runtime, const Runtime::RuntimeSceneView &scene,
                                                  Math::WorldCoordinate64 origin, std::uint64_t tick, ScenePhysicsHost *physics = nullptr,
                                                  const HeadlessNavigationPathScope *navigation = nullptr);

        /** @brief Observes terminal results for one exact live agent and tick without changing query execution. */
        void ArmReplay(AI::AgentHandle agent, std::uint64_t tick) noexcept;
        /** @brief Returns the one observed publication prefix after the query's ordinary owner phases finish. */
        [[nodiscard]] Result<std::span<const AI::DecisionQueryPublication>> ReadReplay(std::uint64_t tick) const;
        /** @brief Identifies the cooked query inventory in stable query ID order and its effective work budget. */
        [[nodiscard]] Sha256Digest ReplayProfile() const noexcept;
        /** @brief Reports whether the frozen query inventory needs a Navigation caller. */
        [[nodiscard]] bool UsesNavigation() const noexcept;

    private:
        struct TickScope final {
            AI::AiSceneRuntime &runtime;
            const Runtime::RuntimeSceneView &scene;
            Math::WorldCoordinate64 origin;
            std::uint64_t tick;
            ScenePhysicsHost *physics;
            const HeadlessNavigationPathScope *navigation;
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
        /** @brief Resolves the compiled native plan into one scoped point or Perception execution. */
        [[nodiscard]] Result<std::unique_ptr<AI::QueryExecution>> AdmitPlan(const CookedSceneQueryPlan &plan,
                                                                            const AI::AiTaskOperationContext &operation,
                                                                            std::uint64_t submitted, std::uint64_t deadline);
        /** @brief Prepares both provider stages against the same frozen candidate context. */
        [[nodiscard]] Result<std::unique_ptr<AI::QueryExecution>> AdmitMixedPointPlan(const AI::EnvironmentQueryPlan &plan,
                                                                                      Runtime::EntityRef querier,
                                                                                      AI::QueryExecutionInputs inputs,
                                                                                      AI::QueryExecutionSettings settings);
        /** @copydoc AI::IQueryPhysicsVisibilityOwner::RetainsVisibilityAuthority */
        [[nodiscard]] bool RetainsVisibilityAuthority(const Runtime::RuntimeSceneView &scene,
                                                      const AI::QueryPhysicsPublication &publication) const noexcept override;
        /** @copydoc AI::IQueryPhysicsVisibilityOwner::CaptureVisibility */
        [[nodiscard]] Result<std::optional<AI::QueryPhysicsVisibilityResult>> CaptureVisibility(
            const AI::QueryPhysicsVisibilityRequest &request) override;
        [[nodiscard]] std::optional<AI::QueryNavigationAccess> ReadQueryNavigation(
            const Runtime::RuntimeSceneView &scene) const noexcept override;
        /** @brief Acquires a revocable capability only when the exact published Scene or Physics world changes. */
        [[nodiscard]] Result<void> PrepareVisibilityCapability(Physics::PhysicsWorld &world);
        /** @brief Publishes at most one result per active query through the Scene-owned BlackboardSync lane. */
        [[nodiscard]] Result<void> PublishOutputs(TickScope &scope);
        /** @brief Issues bounded allowances in round-robin occurrence order without allocating. */
        void AdvanceQueries(std::uint64_t tick);
        /** @brief Allocates retention once per active Scene incarnation. */
        [[nodiscard]] Result<void> PrepareCache(const Runtime::RuntimeSceneView &scene);

        HeadlessQueryPlanCatalog plans_;
        std::shared_ptr<const AI::QuerySchemaRegistry> registry_;
        std::optional<AI::QueryContextProviderRegistry> contexts_;
        std::vector<std::weak_ptr<AI::DecisionQueryTask>> tasks_;
        std::vector<AgentBudget> budgets_;
        std::size_t maximumActive_{};
        std::size_t workUnitsLimit_{};
        std::optional<AI::QueryResultCacheLimits> cacheLimits_;
        std::shared_ptr<AI::QueryResultCache> cache_;
        Physics::PhysicsQueryEventCapability visibility_;
        Runtime::RuntimeSceneView visibilityScene_;
        bool bestEffort_{};
        std::size_t cursor_{};
        TickScope *scope_{};
        AI::AgentHandle replayAgent_;
        std::uint64_t replayTick_{};
        std::array<AI::DecisionQueryPublication, 32> replayPublications_{};
        std::size_t replayCount_{};
        bool replayOverflow_{};
    };
}  // namespace Horo::Application::Internal
