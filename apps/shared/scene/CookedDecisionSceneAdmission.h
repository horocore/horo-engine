#pragma once

/** @file CookedDecisionSceneAdmission.h @brief Shared application-owned verification of committed Scene Decision inputs. */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/LocalLearnedPolicy.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <map>

namespace Horo::AI {
    class QuerySchemaRegistry;
    class EnvironmentQueryPlan;
}  // namespace Horo::AI

namespace Horo::Runtime {
    class RuntimeSceneDefinition;
}

namespace Horo::Application {
    struct ProjectLocalPolicySettings;
}

namespace Horo::Application::Internal {
    class CommittedSceneAssets;

    /** @brief Compiled query and the exact authenticated cooked bytes that produced it. */
    struct CookedSceneQueryPlan final {
        std::shared_ptr<const AI::EnvironmentQueryPlan> plan;
        Sha256Digest digest;
    };

    using CookedSceneQueryPlanCatalog = std::map<AI::QueryId, CookedSceneQueryPlan>;

    /** @brief Explicit immutable model selection and interpreter grants from the composing host. */
    struct CookedSceneLocalPolicySelection final {
        Assets::AssetId artifact;
        AI::LocalPolicyCookTarget target;
        AI::AiCapabilityDescriptorSet tasks;
        std::optional<AI::BlackboardSchemaId> blackboard;
        std::vector<AI::BlackboardKeyId> featureKeys;
    };

    using CookedSceneLocalPolicyCatalog = AI::AiSceneLocalPolicyCatalog;

    /** @brief Durable selection of a Decision artifact for an initial or explicit prefab controller. */
    struct CookedDecisionSceneBinding final {
        AI::ControllerTypeId controller;
        Assets::AssetId artifact;
        std::optional<Assets::AssetId> prefab;
        std::shared_ptr<const CookedSceneLocalPolicySelection> localPolicy;
    };

    /** @brief Applies authored model selections only to the host's explicitly selected Scene controllers. */
    [[nodiscard]] Result<std::vector<CookedDecisionSceneBinding>> ConfigureSceneLocalPolicies(
        std::span<const CookedDecisionSceneBinding> bindings, const ProjectLocalPolicySettings *policy, const AssetCookTargetId &target);

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
     * @param localPolicies Scene-owned output required when any model is selected; assigned only on success.
     * @return Detached descriptor catalog or original typed admission failure.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> AdmitCookedSceneControllers(
        std::span<const AI::AiControllerDescriptor> catalog, const Runtime::RuntimeSceneDefinition &scene,
        const CookedDecisionSceneAdmissionInput &input, const Prefab::PrefabLimitProfile &prefabLimits,
        const CancellationToken &cancellation, CookedSceneQueryPlanCatalog *queryPlans = nullptr,
        CookedSceneLocalPolicyCatalog *localPolicies = nullptr);
}  // namespace Horo::Application::Internal
