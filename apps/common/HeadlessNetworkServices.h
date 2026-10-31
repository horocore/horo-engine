#pragma once

#include "GameplayWorldComposition.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Prefab/PrefabLimits.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "ScenePhysicsHost.h"

#include <memory>

namespace Horo::Application::Internal {
    // Installs the real gameplay-world participants shared by standalone and dedicated products.
    // Network and presentation factories remain explicit application decisions.
    [[nodiscard]] Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories = {},
        const std::optional<GameplayWorldSelection> &gameplay = std::nullopt,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission = std::nullopt);

    /** @brief Explicit application input for real project Scene activation; no policy is inferred from source data.
     * @details The registry and load service (including its running JobSystem/provider) outlive every
     * returned factory and world service. The load boundary is borrowed on that same owner lane;
     * set it once before each complete Startup/Travel operation and never during that synchronous call.
     * Every role/retry in one operation shares its unchanged absolute deadline and cancellation ancestry.
     * The caller issues globally fresh Scene IDs; each record admits exactly its request.scene once.
     */
    struct HeadlessSceneActivationInput final {
        Physics::PhysicsRuntimeMode mode;
        Physics::PhysicsSceneActivationSettings settings;
        Assets::AssetRegistry &registry;
        Assets::AssetLoadService &loads;
        const HeadlessSceneLoadBoundary &load;
        std::span<const AI::AiControllerDescriptor> aiControllers; /**< Immutable explicit catalog, outliving all returned services. */
        AI::AiSceneRuntimeSettings aiSettings{};
        Assets::AssetLoadService *prefabLoads{};  /**< Optional exact-envelope loader; outlives all granted prefab services. */
        Prefab::PrefabProjectPolicy prefabPolicy; /**< Explicit host-admitted policy; declared engine defaults preserve old callers. */
    };

    /** @brief Composes actual project Scene services and their sole aggregate-owned Physics/Character pair.
     * @param definition Validated authored/cooked definition. @param input Explicit project/backend/load authority.
     * @param selectedFactories Existing network/presentation factories. @param gameplay Explicit optional gameplay policy.
     * @return Factories or original selected backend rejection; never substitutes another backend.
     * @details Normal frame phases never block. Startup/travel Prepare drives only the bounded owned asset-load boundary.
     */
    [[nodiscard]] Result<Network::NetworkModeFactories> ComposeHeadlessSceneNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition, HeadlessSceneActivationInput input,
        Network::NetworkModeFactories selectedFactories = {}, const std::optional<GameplayWorldSelection> &gameplay = std::nullopt,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission = std::nullopt);

    /** @brief Inspect the already-bound execution context of an explicitly selected headless world service.
     * @param service Borrowed application-owned service, not a process-global discovery source.
     * @return Read-only context or null for omitted gameplay/non-Physics services; never grants permission.
     */
    [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept;
}  // namespace Horo::Application::Internal
