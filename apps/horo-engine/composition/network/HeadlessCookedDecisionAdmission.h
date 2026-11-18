#pragma once
/** @file HeadlessCookedDecisionAdmission.h
 * @brief Exact application-owned mapping between controller identities and committed decision assets.
 */
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetId.h"

namespace Horo::Runtime {
    class RuntimeSceneDefinition;
}

namespace Horo::Application::Internal {
    class CommittedSceneAssets;

    /** @brief Durable project asset selection for one existing AI controller descriptor. */
    struct HeadlessCookedDecisionBinding final {
        AI::ControllerTypeId controller;
        Assets::AssetId artifact;
    };

    /** @brief Authoritative scene opt-in, captured from the actual project asset and provider registries. */
    struct HeadlessCookedDecisionInput final {
        const CommittedSceneAssets &committed;          /**< Exact immutable generation; outlives this input and all worlds using it. */
        AI::DecisionCompilationEnvironment environment; /**< Empty schema input takes the immutable committed schema catalog. */
        std::vector<HeadlessCookedDecisionBinding> bindings;
        AI::DecisionCodecLimits limits; /**< Per-plan codec limits; compiled bytes also bound total retained unique plans. */
        std::optional<AI::AiSceneCookedDecisionExecution> execution; /**< Explicit authority scheduler; empty only admits assets. */
    };

    /** @brief Builds an immutable staged descriptor catalog from authenticated plans and the exact compiler environment.
     * @param catalog Optional existing controller declarations and provider metadata, including prefab-only bindings.
     * @param scene Exact initial Scene supplying authored controller bindings for explicitly selected controllers.
     * @param input Owning application capture and typed selections. Unselected descriptors keep their existing bindings.
     * @param cancellation Exact startup/Scene owner ancestry sampled before and between decodes.
     * @return Owned descriptors for both initial and structural owners, or original typed admission failure.
     */
    [[nodiscard]] Result<std::vector<AI::AiControllerDescriptor>> AdmitHeadlessCookedControllers(
        std::span<const AI::AiControllerDescriptor> catalog, const Runtime::RuntimeSceneDefinition &scene,
        const HeadlessCookedDecisionInput &input, const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
