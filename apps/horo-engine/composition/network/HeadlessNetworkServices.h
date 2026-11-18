#pragma once

#include "AnimationRuntimeIdentitySource.h"
#include "GameplayWorldComposition.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Prefab/PrefabLimits.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"
#include "NetworkProductComposition.h"
#include "ScenePhysicsHost.h"

#include <functional>
#include <memory>

namespace Horo::Vfx {
    struct VfxExtractionOutcome;
}

namespace Horo::Application::Internal {
    struct ProjectSceneNavigationSelection;
    struct HeadlessCookedDecisionInput;

    /** @brief Exact registered traversal filter and bounded deadline for the standalone best-effort path grant. */
    struct HeadlessNavigationPathPolicy final {
        static constexpr std::uint32_t MaximumDeadlineTicks = 10'000;
        Navigation::NavigationFilterId filter;
        std::uint32_t deadlineTicks{};

        [[nodiscard]] bool IsValid() const noexcept {
            return filter.IsValid() && deadlineTicks != 0 && deadlineTicks <= MaximumDeadlineTicks;
        }
    };
    // Installs the real gameplay-world participants shared by standalone and dedicated products.
    // Network and presentation factories remain explicit application decisions.
    [[nodiscard]] Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories = {},
        const std::optional<GameplayWorldSelection> &gameplay = std::nullopt,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission = std::nullopt,
        NetworkNativeRetirementPoll *retirement = nullptr);

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
        Assets::AssetLoadService *prefabLoads{};   /**< Optional exact-envelope loader; outlives all granted prefab services. */
        Prefab::PrefabMetricHandles prefabMetrics; /**< Explicit process-prebound observation configuration. */
        Prefab::PrefabProjectPolicy prefabPolicy;  /**< Explicit host-admitted policy. */
        AssetCookTargetId animationTarget;         /**< Exact selected committed target, never inferred from runtime source. */
        std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities; /**< One cold host issuer shared by all records/travel. */
        std::shared_ptr<const Runtime::SceneAnimationInputSource>
            animationInputs; /**< Exact optional adjacent-owner snapshot, no new store. */
        AI::AiScenePerceptionPositionSource
            perceptionPositions; /**< Global positions from the supplied candidate/current Scene in ascending entity order. */
        AI::PerceptionFilteringPolicy perceptionFiltering;
        AI::AiScenePerceptionGroupPositionSource perceptionGroupPositions; /**< Exact detached endpoint coordinates at admission. */
        std::shared_ptr<const ProjectSceneNavigationSelection> navigation;
        std::shared_ptr<const HeadlessCookedDecisionInput>
            cookedDecisions; /**< Optional explicit authoritative controller/asset catalog. */
        std::optional<HeadlessNavigationPathPolicy>
            navigationPath; /**< Explicit native scope for the installed immutable Navigation world. */
    };

    /** @brief Composes actual project Scene services and their sole aggregate-owned Physics/Character pair.
     * @param definition Validated authored/cooked definition. @param input Explicit project/backend/load authority.
     * @param selectedFactories Existing network/presentation factories. @param gameplay Explicit optional gameplay policy.
     * @return Factories or original selected backend rejection; never substitutes another backend.
     * @details Normal frame phases never block. Startup/travel Prepare drives only the bounded owned asset-load boundary.
     */
    [[nodiscard]] Result<NetworkProductComposition> ComposeHeadlessSceneNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition, HeadlessSceneActivationInput input,
        Network::NetworkModeFactories selectedFactories = {}, const std::optional<GameplayWorldSelection> &gameplay = std::nullopt,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission = std::nullopt);

    /** @brief Borrows the actual configured Scene's latest immutable VFX frame on its real owner lane.
     * @param service Actual retained Scene/Physics service facade.
     * @return Current frame, or null when unavailable/retired/foreign-lane. Borrow ends at next extraction or Scene teardown.
     */
    [[nodiscard]] const Vfx::VfxExtractionOutcome *InspectHeadlessVfxFrame(const Network::INetworkModeService &service) noexcept;

    /** @brief Borrow canonical Animation observation on the service owner lane; no ownership or mutation grant. */
    [[nodiscard]] const Runtime::SceneAnimationRuntime *InspectHeadlessAnimation(const Network::INetworkModeService &service) noexcept;
    /** @brief Reads the authoritative world's last complete spatial publication on its owner lane. */
    [[nodiscard]] std::shared_ptr<const AI::PerceptionSpatialSnapshot> InspectHeadlessPerception(
        const Network::INetworkModeService &service) noexcept;
    /** @brief Borrows the committed configured Scene for read-only diagnostics on its owner lane.
     * @param service Actual retained Scene/Physics service facade.
     * @return View until the next lifecycle mutation, or no view when absent, closing, or called off-lane.
     */
    [[nodiscard]] std::optional<Runtime::RuntimeSceneView> InspectHeadlessScene(const Network::INetworkModeService &service) noexcept;
    [[nodiscard]] std::optional<Runtime::EntityRef> InspectHeadlessAnimationEntity(const Network::INetworkModeService &service,
                                                                                   Runtime::SceneObjectId object) noexcept;

    /** @brief Reads the already-bound native path scope on its owner lane, when explicitly selected. */
    [[nodiscard]] std::shared_ptr<const Gameplay::GameplayNavigationContext> InspectHeadlessNavigationContext(
        const Network::INetworkModeService &service) noexcept;

    /** @brief Inspect the already-bound execution context of an explicitly selected headless world service.
     * @param service Borrowed application-owned service, not a process-global discovery source.
     * @return Read-only context or null for omitted gameplay/non-Physics services; never grants permission.
     */
    [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept;
}  // namespace Horo::Application::Internal
