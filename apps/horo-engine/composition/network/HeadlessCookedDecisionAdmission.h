#pragma once
/** @file HeadlessCookedDecisionAdmission.h
 * @brief Exact application-owned mapping between controller identities and committed decision assets.
 */
#include "CookedDecisionSceneAdmission.h"
#include "Horo/AI/EnvironmentQuerySchema.h"

#include <functional>

namespace Horo::AI {
    class GameplayQueryTestRegistry;
}

namespace Horo::Application::Internal {
    using HeadlessQueryPlanCatalog = CookedSceneQueryPlanCatalog;
    using HeadlessCookedDecisionBinding = CookedDecisionSceneBinding;

    /** @brief One authority-owned observation keyed by a persistent Scene agent identity. */
    struct HeadlessDecisionRelevance final {
        AI::AgentId agent;
        AI::AiDecisionRelevance relevance;
    };

    /** @brief Samples gameplay facts from the committed Scene on the fixed-step owner lane.
     * @details The bounded result is borrowed until the next invocation. Each identity may appear
     * once. Omitting an agent preserves its last observation. The source must not mutate the Scene.
     */
    using HeadlessDecisionRelevanceSource =
        std::function<Result<std::span<const HeadlessDecisionRelevance>>(const Runtime::RuntimeSceneView &, std::uint64_t)>;

    /** @brief Provider ownership chosen by the composing host before opening any candidate. */
    enum class HeadlessDecisionProvider {
        Captured,
        SelectedNativeGameplay
    };

    /** @brief Authoritative scene opt-in, captured from the actual project asset and provider registries. */
    struct HeadlessCookedDecisionInput final {
        const CommittedSceneAssets &committed;          /**< Exact immutable generation; outlives this input and all worlds using it. */
        AI::DecisionCompilationEnvironment environment; /**< Empty schema input takes the immutable committed schema catalog. */
        std::vector<HeadlessCookedDecisionBinding> bindings;
        AI::DecisionCodecLimits limits; /**< Per-plan codec limits; compiled bytes also bound total retained unique plans. */
        std::optional<AI::AiSceneCookedDecisionExecution> execution; /**< Explicit authority scheduler; empty only admits assets. */
        HeadlessDecisionProvider provider{HeadlessDecisionProvider::Captured};
        std::shared_ptr<const AI::QuerySchemaRegistry> querySchema; /**< Explicit host descriptors for committed query artifacts. */
        std::size_t queryWorkUnitsLimit{AI::AiTaskSchedulerSettings::HardWorkUnits};
        std::optional<AI::QueryResultCacheLimits> queryCacheLimits{};
        std::shared_ptr<const AI::GameplayQueryTestRegistry> gameplayQueryTests;
        HeadlessDecisionRelevanceSource relevance; /**< Optional owner source for a live best-effort simulation profile. */
    };

    /** @brief Builds an immutable staged descriptor catalog from authenticated plans and the exact compiler environment.
     * @param catalog Optional existing controller declarations and provider metadata, including prefab-only bindings.
     * @param scene Exact initial Scene supplying authored controller bindings for explicitly selected controllers.
     * @param input Owning application capture and typed selections. Unselected descriptors keep their existing bindings.
     * @param prefabLimits Exact project limits also used by the runtime template provider.
     * @param cancellation Exact startup/Scene owner ancestry sampled before and between decodes.
     * @param queryPlans Optional output for the immutable plans from the exact same generation, assigned only on success.
     * @return Owned descriptors for both initial and structural owners, or original typed admission failure.
     */
    [[nodiscard]] inline Result<std::vector<AI::AiControllerDescriptor>> AdmitHeadlessCookedControllers(
        std::span<const AI::AiControllerDescriptor> catalog, const Runtime::RuntimeSceneDefinition &scene,
        const HeadlessCookedDecisionInput &input, const Prefab::PrefabLimitProfile &prefabLimits, const CancellationToken &cancellation,
        HeadlessQueryPlanCatalog *queryPlans = nullptr, CookedSceneLocalPolicyCatalog *localPolicies = nullptr) {
        return AdmitCookedSceneControllers(catalog, scene,
                                           {input.committed, input.environment, input.bindings, input.limits, input.querySchema},
                                           prefabLimits, cancellation, queryPlans, localPolicies);
    }
}  // namespace Horo::Application::Internal
