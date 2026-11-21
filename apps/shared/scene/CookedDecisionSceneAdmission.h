#pragma once

/** @file CookedDecisionSceneAdmission.h @brief Shared application-owned verification of committed Scene Decision inputs. */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <map>

namespace Horo::AI {
    class QuerySchemaRegistry;
    class EnvironmentQueryPlan;
}  // namespace Horo::AI

namespace Horo::Runtime {
    class RuntimeSceneDefinition;
}

namespace Horo::Application::Internal {
    class CommittedSceneAssets;

    /** @brief Compiled query and the exact authenticated cooked bytes that produced it. */
    struct CookedSceneQueryPlan final {
        std::shared_ptr<const AI::EnvironmentQueryPlan> plan;
        Sha256Digest digest;
    };

    using CookedSceneQueryPlanCatalog = std::map<AI::QueryId, CookedSceneQueryPlan>;

    /** @brief Durable selection of a Decision artifact for an initial or explicit prefab controller. */
    struct CookedDecisionSceneBinding final {
        AI::ControllerTypeId controller;
        Assets::AssetId artifact;
        std::optional<Assets::AssetId> prefab;
    };

    /** @brief Borrowed verified asset generation and exact inert provider contract used for synchronous admission. */
    struct CookedDecisionSceneAdmissionInput final {
        const CommittedSceneAssets &committed;
        const AI::DecisionCompilationEnvironment &environment;
        std::span<const CookedDecisionSceneBinding> bindings;
        AI::DecisionCodecLimits limits;
        std::shared_ptr<const AI::QuerySchemaRegistry> querySchema;
    };

    /** @brief Builds an immutable descriptor catalog from authenticated plans without activating any provider.
     * @param catalog Existing Controller declarations. @param scene Exact authored Controller topology.
     * @param input Verified asset generation and explicit selections. @param prefabLimits Actual project Prefab limits.
     * @param cancellation Exact activation ancestry. @param queryPlans Optional output assigned only on success.
     * @return Detached descriptor catalog or original typed admission failure.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> AdmitCookedSceneControllers(
        std::span<const AI::AiControllerDescriptor> catalog, const Runtime::RuntimeSceneDefinition &scene,
        const CookedDecisionSceneAdmissionInput &input, const Prefab::PrefabLimitProfile &prefabLimits,
        const CancellationToken &cancellation, CookedSceneQueryPlanCatalog *queryPlans = nullptr);
}  // namespace Horo::Application::Internal
