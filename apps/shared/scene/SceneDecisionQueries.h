#pragma once
/** @file SceneDecisionQueries.h @brief Exact committed query execution for an authoritative Scene owner. */

#include "CookedDecisionSceneAdmission.h"
#include "Horo/AI/DecisionNodeRegistration.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/AI/EnvironmentQueryNavigation.h"
#include "Horo/AI/EnvironmentQueryPhysics.h"

namespace Horo::Physics {
    class PhysicsWorld;
}

namespace Horo::Application::Internal {
    /** @brief Visits each occurrence once and resumes after the first occurrence that spent shared work. */
    template <typename Visitor> std::size_t AdvanceSceneQueryBudgetOrder(const std::size_t count, const std::size_t cursor, Visitor visit) {
        if (count == 0)
            return 0;
        const auto start = cursor % count;
        auto next = start;
        bool spentWork = false;
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto index = (start + offset) % count;
            if (visit(index) && !spentWork) {
                next = (index + 1) % count;
                spentWork = true;
            }
        }
        return next;
    }

    class ScenePhysicsHost;

    /** @brief Synchronous owner-thread projection of the host's committed Navigation authority. */
    struct SceneQueryNavigationSource final {
        const void *owner{};
        std::optional<AI::QueryNavigationAccess> (*read)(const void *, const Runtime::RuntimeSceneView &, Runtime::EntityRef,
                                                         std::uint64_t) noexcept {};
    };

    /** @brief Optional host Decision phase, including its debugger and intent-dispatch behavior. */
    struct SceneQueryDecisionPhase final {
        void *owner{};
        Result<void> (*run)(void *, AI::AiTaskScheduler &, std::uint64_t){};
    };

    /** @brief Exact live agent and cooked document permitted to expose diagnostic samples. */
    struct SceneQueryInspectionSelection final {
        AI::AgentHandle agent;
        AI::QueryId query;
        Sha256Digest cooked;
        bool operator==(const SceneQueryInspectionSelection &) const = default;
    };

    /** @brief Owns the query binding and fixed-step phases for one authoritative runtime world. */
    class SceneDecisionQueries final : public AI::IDecisionQueryTaskBinder,
                                       private AI::IDecisionQueryTaskHost,
                                       private AI::IQueryPhysicsVisibilityOwner,
                                       private AI::IQueryNavigationOwner {
    public:
        /** @brief Freezes the matching query plans and allocates bounded phase state before Scene activation. */
        [[nodiscard]] static Result<std::shared_ptr<SceneDecisionQueries>> Create(
            CookedSceneQueryPlanCatalog plans, std::shared_ptr<const AI::QuerySchemaRegistry> registry,
            const AI::AiTaskSchedulerSettings &settings, std::size_t workUnitsLimit,
            std::optional<AI::QueryResultCacheLimits> cacheLimits = std::nullopt);

        /** @copydoc AI::IDecisionQueryTaskBinder::BindQueryTask */
        [[nodiscard]] Result<std::unique_ptr<AI::IBehaviorTreeExecutor>> BindQueryTask(
            const AI::BehaviorTreeExecutionPlan &tree, const AI::DecisionNodeConfiguration &configuration) override;

        /** @brief Publishes eligible results, evaluates decisions, and advances admitted queries on the same fixed tick. */
        [[nodiscard]] Result<void> RunAtFixedTick(AI::AiSceneRuntime &runtime, const Runtime::RuntimeSceneView &scene,
                                                  Math::WorldCoordinate64 origin, std::uint64_t tick, ScenePhysicsHost *physics = nullptr,
                                                  SceneQueryNavigationSource navigation = {}, SceneQueryDecisionPhase decision = {});

        /** @brief Observes terminal results for one exact live agent and tick without changing query execution. */
        void ArmReplay(AI::AgentHandle agent, std::uint64_t tick) noexcept;
        /** @brief Returns the one observed publication prefix after the query's ordinary owner phases finish. */
        [[nodiscard]] Result<std::span<const AI::DecisionQueryPublication>> ReadReplay(std::uint64_t tick) const;
        /** @brief Identifies the cooked query inventory in stable query ID order and its effective work budget. */
        [[nodiscard]] Sha256Digest ReplayProfile() const noexcept;
        /** @brief Reports whether the frozen query inventory needs a Navigation caller. */
        [[nodiscard]] bool UsesNavigation() const noexcept;
        /** @brief Enables bounded evidence collection for one authenticated query selection. */
        [[nodiscard]] bool SelectInspection(std::optional<SceneQueryInspectionSelection> selection) noexcept;
        /** @brief Transfers the bounded sample from the just-evaluated tick to the host's fixed commit. */
        [[nodiscard]] std::shared_ptr<const AI::QueryDebugSnapshot> TakeInspection() noexcept;

    private:
        struct TickScope final {
            AI::AiSceneRuntime &runtime;
            const Runtime::RuntimeSceneView &scene;
            Math::WorldCoordinate64 origin;
            std::uint64_t tick;
            ScenePhysicsHost *physics;
            SceneQueryNavigationSource navigation;
            std::size_t active{};
        };

        struct AgentBudget final {
            AI::AgentHandle owner;
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
        /** @brief Prepares declared provider stages against one frozen candidate source. */
        [[nodiscard]] Result<std::unique_ptr<AI::QueryExecution>> AdmitPointProviderPlan(const AI::EnvironmentQueryPlan &plan,
                                                                                         Runtime::EntityRef querier,
                                                                                         AI::QueryExecutionInputs inputs,
                                                                                         AI::QueryExecutionSettings settings);
        /** @copydoc AI::IQueryPhysicsVisibilityOwner::RetainsVisibilityAuthority */
        [[nodiscard]] bool RetainsVisibilityAuthority(const Runtime::RuntimeSceneView &scene,
                                                      const AI::QueryPhysicsPublication &publication) const noexcept override;
        /** @copydoc AI::IQueryPhysicsVisibilityOwner::CaptureVisibility */
        [[nodiscard]] Result<std::optional<AI::QueryPhysicsVisibilityResult>> CaptureVisibility(
            const AI::QueryPhysicsVisibilityRequest &request) override;
        [[nodiscard]] std::optional<AI::QueryNavigationAccess> ReadQueryNavigation(const Runtime::RuntimeSceneView &scene,
                                                                                   Runtime::EntityRef querier) const noexcept override;
        /** @brief Acquires a revocable capability only when the exact published Scene or Physics world changes. */
        [[nodiscard]] Result<void> PrepareVisibilityCapability(Physics::PhysicsWorld &world);
        /** @brief Publishes at most one result per active query through the Scene-owned BlackboardSync lane. */
        [[nodiscard]] Result<void> PublishOutputs(TickScope &scope);
        void InspectOutput(const AI::DecisionQueryTask &task, const Runtime::RuntimeSceneView &scene);
        /** @brief Issues bounded allowances in round-robin occurrence order without allocating. */
        void AdvanceQueries(std::uint64_t tick);
        /** @brief Removes retired tasks without advancing past the next stable occurrence. */
        void PruneExpiredTasks();
        /** @brief Allocates retention once per active Scene incarnation. */
        [[nodiscard]] Result<void> PrepareCache(const Runtime::RuntimeSceneView &scene);

        CookedSceneQueryPlanCatalog plans_;
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
        std::optional<SceneQueryInspectionSelection> inspection_;
        std::shared_ptr<const AI::QueryDebugSnapshot> pendingInspection_;
    };
}  // namespace Horo::Application::Internal
