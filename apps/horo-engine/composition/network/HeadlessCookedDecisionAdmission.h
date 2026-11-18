#pragma once
/** @file HeadlessCookedDecisionAdmission.h
 * @brief Exact application-owned mapping between controller identities and committed decision assets.
 */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Prefab/PrefabLimits.h"

namespace Horo::AI {
    class QuerySchemaRegistry;
}

namespace Horo::Runtime {
    class RuntimeSceneDefinition;
}

namespace Horo::Application::Internal {
    class CommittedSceneAssets;

    /** @brief Durable project asset selection for one existing AI controller descriptor. */
    struct HeadlessCookedDecisionBinding final {
        AI::ControllerTypeId controller;
        Assets::AssetId artifact;
        std::optional<Assets::AssetId> prefab; /**< Exact optional committed template that declares a spawn-only controller. */
    };

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
    };

    /** @brief Builds an immutable staged descriptor catalog from authenticated plans and the exact compiler environment.
     * @param catalog Optional existing controller declarations and provider metadata, including prefab-only bindings.
     * @param scene Exact initial Scene supplying authored controller bindings for explicitly selected controllers.
     * @param input Owning application capture and typed selections. Unselected descriptors keep their existing bindings.
     * @param prefabLimits Exact project limits also used by the runtime template provider.
     * @param cancellation Exact startup/Scene owner ancestry sampled before and between decodes.
     * @return Owned descriptors for both initial and structural owners, or original typed admission failure.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> AdmitHeadlessCookedControllers(
        std::span<const AI::AiControllerDescriptor> catalog, const Runtime::RuntimeSceneDefinition &scene,
        const HeadlessCookedDecisionInput &input, const Prefab::PrefabLimitProfile &prefabLimits, const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
