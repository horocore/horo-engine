#pragma once

/** @file GameplayWorldComposition.h
 * @brief Non-installed application composition shared by headless and graphical world owners.
 */

#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Prefab/PrefabSpawnService.h"

namespace Horo::Application::Internal {
    /** @brief Trusted host policy resolved before invoking project code; no ambient grant. */
    enum class GameplayPhysicsPermission {
        Denied,
        Granted
    };
    /** @brief Trusted explicit permission for cooked prefab creation and group retirement. */
    enum class GameplayPrefabPermission {
        Denied,
        Granted
    };

    /** @brief Explicit permission to the already selected Character world; never constructs a consumer. */
    enum class GameplayCharacterPermission {
        Denied,
        Granted
    };

    /** @brief One explicitly selected native module or script and its host-resolved permission. */
    struct GameplayWorldSelection final {
        std::string moduleId;
        std::filesystem::path nativeArtifact;
        std::uint64_t descriptorRevision{};
        std::filesystem::path scriptSource;
        std::filesystem::path scriptSidecar;
        GameplayPhysicsPermission physicsPermission{GameplayPhysicsPermission::Denied};
        bool enabled{true}; /**< Trusted host enablement; disabled modules never execute a callback. */
        GameplayPrefabPermission prefabPermission{GameplayPrefabPermission::Denied};
        std::shared_ptr<Gameplay::GameModuleRetirementQueue>
            moduleRetirements;                                         /**< Explicit application owner, drained before final shutdown. */
        std::shared_ptr<const Runtime::Ui::RuntimeUiClient> runtimeUi; /**< Explicit host grant; absent means typed unavailable. */
        GameplayCharacterPermission characterPermission{GameplayCharacterPermission::Denied};
        std::optional<Gameplay::GameplayReplicationAdmissionPolicy>
            replicationAdmission; /**< Complete host inventory; absent only for non-network hosts. */
    };

    /** @brief Explicit asset authorities and bounds for a composition-owned cooked prefab provider.
     * @details Registry, loader and the Scene service borrow the same host-owned asset authorities. They outlive the
     * composition. This value performs no registration, loading, backend selection or lifecycle work.
     */
    struct GameplayPrefabServices final {
        Assets::AssetRegistry &registry;
        Assets::AssetLoadService &loads;
        Prefab::PrefabLimitProfile profile;
        Prefab::PrefabTemplateProviderLimits limits{};
        CancellationToken cancellation;
    };

    /** @brief Owner-thread gameplay lifetime borrowing an explicit world and Scene.
     * Revoke runs before behavior/module stop and before the owning host retires world/Scene storage.
     * The Physics holder itself retains neither borrowed object. Scripts and native callbacks use
     * the same Physics-owned capability, through their normal execution contexts.
     */
    class GameplayWorldComposition final {
    public:
        /** @brief Activate the selected module/script for an exact Scene activation epoch.
         * @param scene Host-owned Scene outliving this composition.
         * @param world Explicitly selected world; null means unavailable, never fallback.
         * @param sceneGeneration Non-zero host activation epoch used by Physics ticks.
         * @param selection Trusted resolved artifact identity and permission policy.
         * @param character Explicit paired active Character world, or no available consumer; borrowed only during admission.
         * @return Active composition or typed activation failure with complete rollback.
         */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> Create(Runtime::RuntimeScene &scene,
                                                                                      Physics::PhysicsWorld *world,
                                                                                      std::uint64_t sceneGeneration,
                                                                                      const GameplayWorldSelection &selection,
                                                                                      Character::CharacterWorld *character = nullptr);
        /** @brief Activates the production cooked-prefab path against the actual Scene transaction authority.
         * @param scenes Active host Scene service, outliving this composition.
         * @param templates Cooked runtime provider, outliving this composition and borrowing the same Scene service.
         * @param world Explicit optional Physics world.
         * @param sceneGeneration Exact non-zero host activation epoch.
         * @param selection Selected module/script and independent trusted prefab/Physics permissions.
         * @param components Frozen optional custom component registry, outliving this composition.
         * @param character Explicit paired active Character world; no consumer is selected implicitly.
         * @return Active Gameplay/Prefab owner or typed admission/activation failure with rollback.
         * @pre MakeGameplayStructuralParticipant was registered against this host's active composition slot before Scene startup.
         */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> Create(
            Runtime::RuntimeSceneService &scenes, Prefab::PrefabTemplateProvider &templates, Physics::PhysicsWorld *world,
            std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components = nullptr,
            Character::CharacterWorld *character = nullptr);
        /** @brief Owns the cooked provider until module retirement and releases its pins before host asset teardown.
         * @param scenes Active host Scene service, outliving this composition.
         * @param assets Explicit matching asset authorities, immutable policy and provider resource limits.
         * @param world Explicit optional Physics world; no backend is selected.
         * @param sceneGeneration Exact non-zero host activation epoch.
         * @param selection Selected module/script with an explicit Granted prefab permission.
         * @param components Optional frozen custom component registry, outliving this composition.
         * @param character Explicit paired active Character world, or no available consumer.
         * @return Active provider/Gameplay owner or typed failure without retaining module or provider ownership.
         * @pre The host registered MakeGameplayStructuralParticipant against its active slot before Scene startup.
         */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> Create(
            Runtime::RuntimeSceneService &scenes, const GameplayPrefabServices &assets, Physics::PhysicsWorld *world,
            std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components = nullptr,
            Character::CharacterWorld *character = nullptr);
        /** @brief Activates gameplay against the real Scene service when this host selects no runtime prefab provider.
         * @param scenes Active single Scene authority outliving the composition.
         * @param world Explicit paired Physics owner or null for unavailable access.
         * @param sceneGeneration Exact nonzero activation epoch shared with this Physics owner.
         * @param selection Trusted module/script and capability selection; a prefab grant is rejected before callbacks.
         * @param character Explicit paired Character owner or null; borrowed through admission only.
         * @return Active gameplay owner or typed admission failure without discovering a provider or cloning the Scene.
         * @pre The host registers MakeGameplayStructuralParticipant before Scene startup and retires gameplay before Scene.
         */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> Create(Runtime::RuntimeSceneService &scenes,
                                                                                      Physics::PhysicsWorld *world,
                                                                                      std::uint64_t sceneGeneration,
                                                                                      const GameplayWorldSelection &selection,
                                                                                      Character::CharacterWorld *character = nullptr);
        ~GameplayWorldComposition();
        GameplayWorldComposition() = default;
        GameplayWorldComposition(const GameplayWorldComposition &) = delete;
        GameplayWorldComposition &operator=(const GameplayWorldComposition &) = delete;
        /** @brief Dispatch the canonical fixed Gameplay phase. @return First callback/transaction failure. */
        [[nodiscard]] Result<void> FixedUpdate(Gameplay::FixedDeltaTime delta);
        /** @brief Advances exact host tick addressing then invokes Gameplay callbacks. @param context Current host tick.
         * @return Tick/callback failure; admission is revoked on callback failure. */
        [[nodiscard]] Result<void> FixedTick(const Runtime::FixedStepContext &context);
        /** @brief Advances prefab preparation, commits the actual Scene transaction, and observes its completion.
         * @param phase Host lifecycle phase.
         * @param context Current immutable frame context.
         * @return Typed owner failure. This composition owns Scene safe-point dispatch for the service-backed path.
         */
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context);
        /** @brief Revoke the whole world scope before any shutdown callback or storage retirement. */
        void Shutdown() noexcept;
        /** @brief Retire just the module/script; the explicit Physics world may stay active. */
        void UnloadModule() noexcept;
        /** @brief Borrow the immutable Physics execution context for integrated host diagnostics. */
        [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept;
        /** @brief Reads the selected module Character scope, including typed unavailable diagnostics. @return Owned holder. */
        [[nodiscard]] std::shared_ptr<const Gameplay::GameplayCharacterContext> CharacterContext() const noexcept;
        /** @brief Borrows this active runner for an aggregate Scene group transaction.
         * @return Gameplay participant or null when the selected module is not active.
         * @pre This composition outlives the returned adapter and its prepared candidates.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant();

    private:
        /** @brief Captures native component metadata before activating any service-backed behavior callbacks.
         * @param scenes Published Scene authority, outliving this composition.
         * @param components Explicit optional frozen project metadata; otherwise the loaded module supplies the owned snapshot.
         * @return Activated runner or original component/runtime admission failure.
         */
        [[nodiscard]] Result<void> ActivateServiceRunner(Runtime::RuntimeSceneService &scenes,
                                                         const Gameplay::ComponentRegistry *components);

        /** @brief Host-owned inert adapter; runtime Prefab code never depends on the Scene source codec. */
        class BuiltinProjector final : public Prefab::PrefabComponentProjector {
        public:
            [[nodiscard]] Result<bool> Project(const Prefab::RawComponentPayload &source, Runtime::RuntimeComponentSet &destination,
                                               std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh) const override;
        };

        /** @brief Shares exact Scene-service construction; nullable templates express the host's explicit provider selection. */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> CreateServiceBacked(
            Runtime::RuntimeSceneService &scenes, Prefab::PrefabTemplateProvider *templates, Physics::PhysicsWorld *world,
            std::uint64_t sceneGeneration, const GameplayWorldSelection &selection, const Gameplay::ComponentRegistry *components,
            Character::CharacterWorld *character);
        /** @brief Validates artifacts and activates callbacks only after Physics admission is resolved. */
        [[nodiscard]] Result<void> Activate(Runtime::RuntimeScene &scene, const GameplayWorldSelection &selection);
        /** @brief Loads the explicitly selected module/script and freezes its registry before any callback. */
        [[nodiscard]] Result<void> RegisterModule(const GameplayWorldSelection &selection);
        /** @brief Loads and contributes one native generation with pre-Start replication admission and retained rollback ownership.
         * @param selection Exact artifact, host policy and explicit retirement owner.
         * @return Original load/admission/contribution failure, or a contributed native generation.
         */
        [[nodiscard]] Result<void> RegisterNativeModule(const GameplayWorldSelection &selection);
        /** @brief Pins and admits the loaded generation before any behavior callback. @param selection Complete host policy.
         * @return Original acquisition or declaration failure; native pre-Start admission already ran in its loader. */
        [[nodiscard]] Result<void> AdmitReplication(const GameplayWorldSelection &selection);
        std::shared_ptr<Gameplay::GameplayPhysicsContext> physics_;
        std::shared_ptr<const Runtime::Ui::RuntimeUiClient> runtimeUi_;
        std::shared_ptr<Gameplay::GameplayCharacterContext> character_;
        std::unique_ptr<Gameplay::LoadedGameModule> native_;
        std::shared_ptr<Gameplay::GameModuleRetirementQueue> moduleRetirements_;
        std::optional<Gameplay::GameModuleRetirementQueue::Reservation> retirementAdmission_;
        std::unique_ptr<Gameplay::LuaBehaviorProgram> script_;
        std::optional<Gameplay::GameplayReplicationLease> replicationLease_; /**< Released after callbacks and before native retirement. */
        Gameplay::BehaviorRegistry registry_;
        std::unique_ptr<Gameplay::BehaviorRuntime> behaviors_;
        std::unique_ptr<Prefab::PrefabTemplateProvider> ownedTemplates_;
        BuiltinProjector builtinProjector_;            /**< Outlives spawn shutdown and every synchronous projection using it. */
        Gameplay::ComponentRegistry prefabComponents_; /**< Owned inert metadata, frozen before behavior activation; outlives spawn. */
        std::unique_ptr<Prefab::PrefabSpawnService> prefabs_;
        std::shared_ptr<Gameplay::GameplayPrefabContext> prefabContext_;
        Runtime::RuntimeSceneService *scenes_{};
        std::uint64_t tick_{};
    };

    /** @brief Composes the Gameplay structural owner before Scene startup using an explicit application-owned active slot.
     * @param active Host slot populated only after successful module/scene activation, retired at drained lifecycle safe points.
     * @return Owned adapter; the slot and its current composition outlive each synchronous group transaction.
     * @details Empty/retired slots reject required Gameplay work rather than discover or load another module.
     */
    [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeGameplayStructuralParticipant(
        GameplayWorldComposition *const &active);
    /** @brief Rejects temporary pointer slots that would dangle before the returned participant is used. */
    std::unique_ptr<Runtime::SceneStructuralParticipant> MakeGameplayStructuralParticipant(GameplayWorldComposition *&&active) = delete;
}  // namespace Horo::Application::Internal
