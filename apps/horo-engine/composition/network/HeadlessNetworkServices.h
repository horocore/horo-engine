#pragma once

#include "AnimationRuntimeIdentitySource.h"
#include "GameplayWorldComposition.h"
#include "HeadlessSceneLoadBoundary.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationCrowdAvoidance.h"
#include "Horo/Navigation/NavigationMovement.h"
#include "Horo/Network/NetworkModeComposition.h"
#include "Horo/Network/SceneReplicationCommitSource.h"
#include "Horo/Prefab/PrefabLimits.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"
#include "Horo/WorldStreaming/OriginFrame.h"
#include "NetworkProductComposition.h"
#include "ScenePhysicsHost.h"

#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::Render {
    class RenderOriginSceneAdapter;
}

namespace Horo::Runtime {
    class PrimitiveMeshCache;
}

namespace Horo::Vfx {
    struct VfxExtractionOutcome;
}

namespace Horo::AI {
    class AiTaskContinuation;
}

namespace Horo::Application::Internal {
    namespace HeadlessNetworkDetail {
        struct WorldRecord;
        class WorldService;
        class HeadlessFixedParticipant;
    }  // namespace HeadlessNetworkDetail

    struct ProjectSceneNavigationSelection;
    struct HeadlessPerceptionPolicy;
    struct CommittedWorldSelection;
    struct CommittedWorldInitialCell;
    class MountedWorldRenderPacket;

    /** @brief Selects the sole descriptor authority for an authored Perception policy. */
    enum class HeadlessPerceptionProvider {
        CapturedRegistry,
        SelectedNativeGameplay,
    };

    /** @brief Exact authored selection and descriptor authority granted by the configured host. */
    struct HeadlessPerceptionProjectAdmission final {
        std::shared_ptr<const HeadlessPerceptionPolicy> policy;
        std::shared_ptr<const AI::PerceptionDescriptorRegistry> registry;
        HeadlessPerceptionProvider provider{HeadlessPerceptionProvider::CapturedRegistry};
    };
    struct HeadlessCookedDecisionInput;

    /** @brief Explicit one-shot diagnostic selection for a configured standalone World. */
    struct HeadlessDecisionReplaySelection final {
        AI::AgentId agent;
        std::uint64_t tick{};

        [[nodiscard]] bool IsValid() const noexcept {
            return agent.IsValid() && tick != 0;
        }
    };

    /** @brief Achieved motion keyed by an original authored Scene controller, independent of process handles. */
    struct HeadlessReplayCharacterMotion final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsComponentId component;
        std::uint64_t characterTick{};
        Math::Vec3 position;
        Math::Quaternion heading;
        Math::Vec3 achievedVelocity;
        Character::CharacterMovementTermination termination{};
        bool grounded{};
    };

    /** @brief Native rigid-body observation without world handles or solver cache state. */
    struct HeadlessReplayRigidBody final {
        Runtime::SceneObjectId object;
        Runtime::PhysicsBodySlotId slot;
        Physics::PhysicsPose pose;
        Math::Vec3 linearVelocity;
        Math::Vec3 angularVelocity;
        Physics::PhysicsBodyActivity activity;
    };

    /** @brief Exact native publication of the authored rigid-body projection in one fixed attempt. */
    struct HeadlessReplayPhysics final {
        Physics::PhysicsPublishedTick publication;
        Physics::PhysicsWorldSettingsIdentity settings;
        Character::CharacterWorldSettingsIdentity characterSettings;
        std::vector<HeadlessReplayRigidBody> bodies;
    };

    /** @brief Exact frozen Perception spatial publication, projected onto authored entity identities. */
    struct HeadlessReplayPerception final {
        std::uint64_t revision{};
        Sha256Digest digest;
        std::size_t sourceCount{};
        std::size_t listenerCount{};
    };

    /** @brief Selected agent's observed query results and Blackboard writes for a single committed tick. */
    struct HeadlessReplayQueries final {
        Sha256Digest profile;
        std::vector<AI::DecisionQueryPublication> publications;
    };

    /** @brief Original decision evidence and its reference-aware canonical encoding owned after fixed commit. */
    struct HeadlessDecisionReplayData final {
        AI::AiSceneDecisionReplayCapture evidence;
        std::vector<std::uint8_t> canonicalRecord;
        Runtime::SceneDefinitionId definition;
        Runtime::SceneDefinitionRevision definitionRevision;
        Sha256Digest sceneInput;
        std::optional<std::vector<HeadlessReplayCharacterMotion>>
            characterMotion;                          /**< Empty records still cover an idle controller tick. */
        std::optional<HeadlessReplayPhysics> physics; /**< Absent for explicit Null Physics. */
        std::optional<HeadlessReplayPerception> perception;
        std::optional<HeadlessReplayQueries> queries;
    };

    /** @brief Product-owned mailbox for one exact committed tick; no concurrent access or file storage. */
    class HeadlessDecisionReplay final {
    public:
        static constexpr std::size_t CanonicalBudget = 1U << 20U;
        using Capture = Result<HeadlessDecisionReplayData>;

        explicit HeadlessDecisionReplay(HeadlessDecisionReplaySelection selection) noexcept : selection_(selection) {}

        HeadlessDecisionReplay(const HeadlessDecisionReplay &) = delete;
        HeadlessDecisionReplay &operator=(const HeadlessDecisionReplay &) = delete;

        /** @brief Reports the immutable selected subject and fixed tick. */
        [[nodiscard]] const HeadlessDecisionReplaySelection &Selection() const noexcept {
            return selection_;
        }

        /** @brief Takes the one published capture or diagnostic on the fixed owner's lane. */
        [[nodiscard]] std::optional<Capture> TakeCommitted() noexcept;

    private:
        friend class HeadlessNetworkDetail::HeadlessFixedParticipant;
        friend class HeadlessNetworkDetail::WorldService;
        /** @brief Captures the exact original root Scene bindings before mounted or gameplay entities materialize. */
        static Result<void> BindAuthoredScene(HeadlessNetworkDetail::WorldRecord &world, const Runtime::RuntimeSceneDefinition &definition);
        void Arm(HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context);
        void Prepare(HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context);
        void Commit(const Runtime::FixedStepContext &context) noexcept;
        void Abort(const Runtime::FixedStepContext &context) noexcept;
        /** @brief Clears only the exact opt-in held by the pinned Scene owner. */
        void DiscardArm() noexcept;

        HeadlessDecisionReplaySelection selection_;
        const std::thread::id owner_{std::this_thread::get_id()};
        const HeadlessNetworkDetail::WorldRecord *attemptWorld_{};
        AI::AgentHandle armedAgent_;
        std::uint64_t attempt_{};
        std::optional<std::uint64_t> previousPhysicsTick_;
        std::optional<HeadlessReplayPerception> perception_;
        std::optional<Capture> pending_;
        std::optional<Capture> committed_;
        bool published_{};
    };

    /** @brief Exact registered traversal filter and bounded deadline for an authority world's best-effort path grant. */
    struct HeadlessNavigationPathPolicy final {
        static constexpr std::uint32_t MaximumDeadlineTicks = 10'000;
        Navigation::NavigationFilterId filter;
        std::uint32_t deadlineTicks{};

        [[nodiscard]] bool IsValid() const noexcept {
            return filter.IsValid() && deadlineTicks != 0 && deadlineTicks <= MaximumDeadlineTicks;
        }
    };

    /** @brief Host-selected authored actor and its dedicated Character motion producer. */
    struct HeadlessNavigationActorPolicy final {
        Runtime::SceneObjectId object;
        std::uint64_t principal{};
        std::optional<Animation::GraphParameterId> motionStartTrigger; /**< Authored trigger when Navigation accepts a new task. */
    };

    /** @brief Explicit project-selected crowd authority, fixed envelope and live Follow observations. */
    struct HeadlessNavigationMotionPolicy final {
        std::shared_ptr<Navigation::INavigationCrowdBackend> solver;
        Navigation::NavigationAvoidanceRequest limits;
        std::function<Result<Navigation::NavigationCrowdSnapshot>(const Runtime::RuntimeSceneView &,
                                                                  const Navigation::NavigationWorldActivationDescriptor &,
                                                                  const Navigation::NavigationAgentSnapshot &,
                                                                  std::span<const Navigation::NavigationCrowdMotionSample>, std::uint64_t)>
            capture;
        std::function<Result<std::optional<Navigation::NavigationMovementTarget>>(const Runtime::RuntimeSceneView &,
                                                                                  Navigation::NavigationAgentOwner, std::uint64_t)>
            follow;
    };

    /** @brief Maps one explicitly public authored action to a stable positive presentation tag. */
    struct HeadlessAnimationActionTag final {
        Animation::GraphActionId action;
        std::int32_t tag{};
    };

    /** @brief Exact graph and slot allowed to contribute an action tag to the selected schema. */
    struct HeadlessAnimationActionSelection final {
        Animation::AnimationGraphId graph;
        Animation::GraphActionSlotId slot;
        std::vector<HeadlessAnimationActionTag> tags;
    };

    /** @brief Explicit installed schema and public Scene fields for authority capture. */
    struct HeadlessSceneTransformReplication final {
        Network::ReplicationSchemaId schema;
        Network::ReplicationSchemaVersion version;
        std::vector<Network::SceneReplicationFieldBinding> fields;
        std::optional<HeadlessAnimationActionSelection> action;
    };

    /** @brief Opts one authored actor into a public scalar input on its presentation graph. */
    struct HeadlessReplicaAnimationBinding final {
        Runtime::SceneObjectId object;
        Network::FieldId field;
        Animation::GraphParameterId parameter;
    };

    class IHeadlessInitialStateAuthority;
    class IHeadlessInitialReplicaAuthority;
    class HeadlessInitialStateHost;

    /** @brief Existing host grant for one exact cooked Scene and selected public replication schema. */
    struct HeadlessInitialStateSelection final {
        Assets::AssetId scene;
        HeadlessSceneTransformReplication transforms;
        std::vector<Network::NetworkObjectProvenance> authoredObjects;
        std::shared_ptr<const IHeadlessInitialStateAuthority> authority;
        std::shared_ptr<const IHeadlessInitialReplicaAuthority> replica;
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
        AssetCookTargetId prefabTarget;            /**< Exact selected local cook target for replica loading. */
        AssetCookTargetId animationTarget;         /**< Exact selected committed target, never inferred from runtime source. */
        std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities; /**< One cold host issuer shared by all records/travel. */
        std::shared_ptr<const Runtime::SceneAnimationInputSource>
            animationInputs; /**< Optional gameplay snapshot for authority and standalone Worlds. */
        std::shared_ptr<const Runtime::SceneAnimationInputSource>
            replicaAnimationInputs; /**< Optional public presentation snapshot for a connected client World. */
        std::vector<HeadlessReplicaAnimationBinding>
            replicaAnimationBindings; /**< Optional public Scene fields, exclusive with an external replica input source. */
        AI::AiScenePerceptionPositionSource
            perceptionPositions; /**< Global positions from the supplied candidate/current Scene in ascending entity order. */
        AI::PerceptionFilteringPolicy perceptionFiltering;
        AI::AiScenePerceptionGroupPositionSource perceptionGroupPositions; /**< Exact detached endpoint coordinates at admission. */
        std::shared_ptr<const ProjectSceneNavigationSelection> navigation;
        std::shared_ptr<const HeadlessCookedDecisionInput>
            cookedDecisions; /**< Optional explicit authoritative controller/asset catalog. */
        std::optional<HeadlessNavigationPathPolicy>
            navigationPath; /**< Explicit native scope for the installed immutable Navigation world. */
        std::optional<AI::AiScenePerceptionExecution>
            perceptionExecution; /**< Frozen native producer and explicit authoritative spatial execution policy. */
        std::optional<HeadlessPerceptionProjectAdmission>
            perceptionProject; /**< Explicit project-backed endpoint and coordinate policy for the selected cooked definition. */
        std::shared_ptr<const CommittedWorldSelection> mountedWorld; /**< Optional exact standalone World cook selection. */
        const JobSystem *worldJobs{}; /**< Existing CPU worker owner, required when live mounted work is selected. */
        std::vector<Network::NetworkObjectProvenance>
            replicatedSceneObjects; /**< Explicit stable object/schema bindings for this definition. */
        std::optional<HeadlessSceneTransformReplication> publicTransforms;
        std::shared_ptr<HeadlessInitialStateHost> initialSceneState;
        std::shared_ptr<HeadlessDecisionReplay> decisionReplay; /**< Optional explicit one-shot standalone replay capture. */
        std::optional<Runtime::SceneAnimationParallelEvaluation> animationParallel;
        std::vector<HeadlessNavigationActorPolicy> navigationActors;            /**< Explicit authoritative AI movement population. */
        std::shared_ptr<const HeadlessNavigationMotionPolicy> navigationMotion; /**< Bounded owner-lane movement policy, when selected. */
    };

    /** @brief Composes actual project Scene services and their sole aggregate-owned Physics/Character pair.
     * @param definition Validated authored/cooked definition. @param input Explicit project/backend/load authority.
     * @param selectedFactories Existing network/presentation factories. @param gameplay Explicit optional gameplay policy.
     * @return Factories or original selected backend rejection; never substitutes another backend.
     * @details Startup/travel Prepare drives the bounded owned asset-load boundary. An explicitly selected dedicated animation
     * policy joins worker-only CPU work before that world's native fixed step.
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
    /** @brief Queues an action through the exact active authoritative Scene facade on its owner lane. */
    [[nodiscard]] Animation::GraphActionCommandOutcome QueueHeadlessAnimationAction(Network::INetworkModeService &service,
                                                                                    Runtime::EntityRef actor,
                                                                                    const Animation::GraphActionRequest &request);
    /** @brief Acquires the configured authority world's exact committed transform producer for an existing capture coordinator.
     * @param service Retained world service facade. @return Read-only producer, absent on client/standalone/unconfigured/retired worlds. */
    [[nodiscard]] std::shared_ptr<const Network::SceneReplicationCommitSource> AcquireHeadlessPublicTransforms(
        const Network::INetworkModeService &service) noexcept;
    /** @brief Reads the authoritative world's last complete spatial publication on its owner lane. */
    [[nodiscard]] std::shared_ptr<const AI::PerceptionSpatialSnapshot> InspectHeadlessPerception(
        const Network::INetworkModeService &service) noexcept;
    /** @brief Reads bounded memory for a recipient from the configured authoritative world's owner lane.
     * @param service Actual retained Scene/Physics service facade. @param recipient Exact live entity generation.
     * @return Owned report or typed unavailable/stale failure.
     */
    [[nodiscard]] Result<AI::PerceptionMemorySnapshot> InspectHeadlessPerceptionMemory(const Network::INetworkModeService &service,
                                                                                       Runtime::EntityRef recipient);
    /** @brief Reads a detached typed Blackboard value from the current authoritative world. */
    [[nodiscard]] Result<std::optional<AI::BlackboardValue>> InspectHeadlessDecisionValue(const Network::INetworkModeService &service,
                                                                                          AI::AgentId agent, AI::BlackboardKeyId key);

    /** @brief Reads the authoritative world's copied Decision failure through the current owner lane. */
    [[nodiscard]] Result<std::optional<AI::AiRuntimeDecisionDiagnostic>> InspectHeadlessDecisionFailure(
        const Network::INetworkModeService &service);

    /** @brief Borrows the committed configured Scene for read-only diagnostics on its owner lane.
     * @param service Actual retained Scene/Physics service facade.
     * @return View until the next lifecycle mutation, or no view when absent, closing, or called off-lane.
     */
    [[nodiscard]] std::optional<Runtime::RuntimeSceneView> InspectHeadlessScene(const Network::INetworkModeService &service) noexcept;
    [[nodiscard]] std::optional<Runtime::EntityRef> InspectHeadlessAnimationEntity(const Network::INetworkModeService &service,
                                                                                   Runtime::SceneObjectId object) noexcept;

    /** @brief Observe the committed World origin through an exact active Scene service.
     * @param service Actual headless Scene owner. @return Generation lease when the selected mount is active.
     */
    [[nodiscard]] std::optional<WorldStreaming::OriginFrameLease> InspectHeadlessWorldOrigin(const Network::INetworkModeService &service);

    /** @brief Requests an exact registered CoreEcs cell through the opted-in standalone Scene owner on its lane.
     * @param service Current standalone Scene facade whose lifetime bounds this request.
     * @param selected Exact committed cell and authored occurrence.
     * @param cancellation Optional parent cancellation retained through the live operation.
     * @return Success after demand admission or an originating typed denial.
     */
    [[nodiscard]] Result<void> RequestHeadlessWorldCell(const Network::INetworkModeService &service,
                                                        const CommittedWorldInitialCell &selected,
                                                        const CancellationToken &cancellation = {});

    /** @brief Withdraws current live demand through the exact standalone Scene owner on its lane.
     * @param service Current standalone Scene facade. @return Success after withdrawal or a typed denial.
     */
    [[nodiscard]] Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service);

    /** @brief Withdraws an exact pending or published occurrence through the standalone Scene owner.
     * @param service Current standalone Scene facade.
     * @param occurrence Exact World occurrence ID originally admitted by RequestHeadlessWorldCell.
     * @return Success after withdrawal or a typed denial.
     */
    [[nodiscard]] Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service, Runtime::SceneInstanceId occurrence);

    /** @brief Captures an exact active World camera and procedural geometry on the standalone Scene owner lane.
     * @param service Actual headless Scene service.
     * @param camera Exact mounted object with an authored camera target.
     * @param meshes Required mounted procedural object identities in ascending order.
     * @param primitives Existing CPU primitive cache.
     * @param renderer Exact mounted World's portable renderer adapter.
     * @return Owned packet or typed failure. The private MountedWorld header defines the owning packet type.
     */
    [[nodiscard]] Result<MountedWorldRenderPacket> CaptureHeadlessWorldRenderGeometry(const Network::INetworkModeService &service,
                                                                                      Runtime::SceneObjectId camera,
                                                                                      std::span<const Runtime::SceneObjectId> meshes,
                                                                                      Runtime::PrimitiveMeshCache &primitives,
                                                                                      Render::RenderOriginSceneAdapter &renderer);

    /** @brief Reads the already-bound native path scope on its owner lane, when explicitly selected. */
    [[nodiscard]] std::shared_ptr<const Gameplay::GameplayNavigationContext> InspectHeadlessNavigationContext(
        const Network::INetworkModeService &service) noexcept;

    /** @brief Stages an exact running decision node's MoveTo or Follow within the configured decision evaluation.
     * @param service Borrowed service for this exact authoritative World.
     * @param task Task continuation from the current behavior-tree callback. @param request Typed movement selection.
     * @param tick Current evaluation tick. @return Admission or typed phase, capability or capacity failure.
     */
    [[nodiscard]] Result<void> StageHeadlessNavigationMotion(const Network::INetworkModeService &service, AI::AiTaskContinuation task,
                                                             Navigation::NavigationMovementRequest request, std::uint64_t tick);

    /** @brief Inspect the already-bound execution context of an explicitly selected headless world service.
     * @param service Borrowed application-owned service, not a process-global discovery source.
     * @return Read-only context or null for omitted gameplay/non-Physics services; never grants permission.
     */
    [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept;
}  // namespace Horo::Application::Internal
