#pragma once

/**
 * @file AISceneActivation.h
 * @brief Transactional SceneRuntime activation and lifecycle ownership for typed AI components.
 */

#include "Horo/AI/AIReplayEvidence.h"
#include "Horo/AI/AIRuntimeCapability.h"
#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/AITaskLifecycle.h"
#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/AiDecisionTaskReservation.h"
#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/LocalLearnedPolicy.h"
#include "Horo/AI/PerceptionDescriptorRegistry.h"
#include "Horo/AI/PerceptionMemory.h"
#include "Horo/Foundation/Time.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    class AiSceneRuntime;
    class DecisionQueryTask;
    struct DecisionQueryPublication;
    class IDecisionQueryTaskBinder;
    class PerceptionSpatialBroadphase;
    class PerceptionSpatialSnapshot;
    class PerceptionSenseGeneration;
    struct PerceptionQueryInput;

    /** @brief Host-captured absolute position of an exact live Perception endpoint. */
    struct AiScenePerceptionPosition final {
        Runtime::EntityRef entity;
        Math::WorldCoordinate64 position;
    };

    /** @brief Captures absolute positions for a Scene's declared endpoints in ascending entity order. */
    using AiScenePerceptionPositionSource =
        std::function<Result<std::vector<AiScenePerceptionPosition>>(const Runtime::RuntimeSceneView &)>;

    /** @brief Admission and fixed-tick service policy for native Scene perception. */
    enum class AiScenePerceptionScheduling : std::uint8_t {
        FullPopulation,   /**< Every admitted listener is reserved on every tick. */
        RotateUnderBudget /**< Per-agent and per-listener turns share a bounded Scene budget. */
    };

    /** @brief Explicit execution policy for a Scene's already selected frozen native producer generation. */
    struct AiScenePerceptionExecution final {
        std::shared_ptr<const PerceptionSenseGeneration> generation;
        PerceptionCapabilitySet capabilities;
        PerceptionMemoryPolicy memory;
        std::uint64_t radiusMillimeters{};
        std::uint64_t visibleLayers{};
        std::size_t workUnitsPerTick{};
        AiScenePerceptionScheduling scheduling{AiScenePerceptionScheduling::FullPopulation};
    };

    /** @brief Explicit perception publication owned by an AI Scene activation candidate. */
    struct AiScenePerceptionActivationInput final {
        PerceptionSpatialBroadphase &spatial; /**< Outlives the Scene participant and all its candidates. */
        PerceptionFilteringPolicy filtering;
        AiScenePerceptionPositionSource positions;           /**< Captures the actual candidate Scene's global positions. */
        std::optional<AiScenePerceptionExecution> execution; /**< Explicit producer grant over the same published endpoints. */
    };

    /** @brief Captures absolute positions for each exact prospective Perception endpoint in ascending entity order. */
    using AiScenePerceptionGroupPositionSource =
        std::function<Result<std::vector<AiScenePerceptionPosition>>(const Runtime::RuntimeSceneView &,
                                                                     std::span<const Runtime::RuntimeEntityView>)>;

    /** @brief Captures detached endpoints using the same candidate hierarchy that will publish with them. */
    using AiScenePerceptionHierarchyPositionSource =
        std::function<Result<std::vector<AiScenePerceptionPosition>>(const Runtime::RuntimeSceneView &,
                                                                     std::span<const Runtime::RuntimeEntityView>,
                                                                     const Runtime::SceneStructuralHierarchyView &)>;

    /** @brief Explicit host composition for publishing spatial endpoints with the Scene structural transaction. */
    struct AiScenePerceptionGroupInput final {
        PerceptionSpatialBroadphase &spatial; /**< Owner outliving the structural participant and its candidates. */
        PerceptionFilteringPolicy filtering;
        AiScenePerceptionGroupPositionSource positions;
        AiScenePerceptionHierarchyPositionSource positionsWithHierarchy; /**< Used when prospective ancestors are needed. */
    };

    /** @brief Exact AI and RuntimeScene incarnation used by one detached activation candidate. */
    struct AiSceneActivationBinding final {
        AiRuntimeIncarnation incarnation;
        Runtime::SceneRuntimeId scene;

        /** @brief Checks that the AI and Scene identities are both usable. @return True for a valid binding. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return incarnation.IsValid() && scene.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const AiSceneActivationBinding &) const noexcept = default;
    };

    /** @brief Complete typed AI payload associated with one exact runtime entity generation. */
    struct AiSceneAgentDescriptor final {
        Runtime::EntityRef owner;
        AiAgentComponent agent;
        std::optional<AiControllerComponent> controller;
    };

    /** @brief Lifecycle state of an admitted scene-owned AI agent. */
    enum class AiAgentActivationState : std::uint8_t {
        Active,
        Disabled,
        Retired,
        Degraded, /**< Host retained authored bindings without an available controller descriptor or execution authority. */
        Count,
    };

    /** @brief Immutable inspection record for one active or recently disabled AI agent slot. */
    struct AiAgentRuntimeRecord final {
        AgentHandle handle;
        Runtime::EntityRef owner;
        AiAgentComponent agent;
        std::optional<AiControllerComponent> controller;
        AiCapabilitySet stagedCapabilities;
        AiAgentActivationState state{AiAgentActivationState::Active};
        bool hasBlackboard{};
        bool hasRunningTask{};
    };

    /** @brief First fatal or advisory Decision failure from the last completed tick of an exact Scene publication. */
    struct AiRuntimeDecisionDiagnostic final {
        std::uint64_t tick{};
        AiSchedulingFailureOrigin origin;
        Error error;
        std::optional<Runtime::EntityRef> entity; /**< Present only while the original agent and entity generations are live. */
    };

    /** @brief Read disposition of one key in a detached, permission-filtered inspector snapshot. */
    enum class AiInspectionValueStatus : std::uint8_t {
        Redacted,
        Unset,
        Present,
        Unavailable
    };

    /** @brief One schema-typed inspector row. Denied, missing, opaque and revoked values carry no payload. */
    struct AiInspectionBlackboardRow final {
        BlackboardKeyId key;
        BlackboardValueKind kind;
        BlackboardValueCardinality cardinality;
        AiInspectionValueStatus status;
        std::optional<BlackboardValue> value;
    };

    /** @brief Typed failure identity with no message, diagnostic payload, or provider-owned cause. */
    struct AiInspectionFailure final {
        ErrorDomainId domain;
        ErrorCode code;
    };

    /** @brief Bounded copied task observation with no authority to resume or cancel work. */
    struct AiInspectionTask final {
        TaskId definition;
        TaskHandle occurrence;
        AiTaskState state;
        std::optional<AiInspectionFailure> failure;
    };

    /** @brief Memory identification and fixed-time age without sensory payloads or coordinates. */
    struct AiInspectionStimulus final {
        PerceptionMemoryKey key;
        std::uint64_t firstSensedTick{};
        std::uint64_t lastSensedTick{};
        double ageSeconds{};
        bool sensed{};
    };

    /** @brief Read-only metadata and liveness of one admitted Perception provider instance. */
    struct AiInspectionSenseProvider final {
        SenseTypeId sense;
        PerceptionDescriptorOrigin origin;            /**< Stable provider identity, contribution family and contract version. */
        PerceptionCapabilitySet requiredCapabilities; /**< Requirements of the registered sense and its stimulus. */
        bool available{};                             /**< Active for this agent when the inspection was captured. */
    };

    /** @brief Entirely owned inspection data scoped to an exact Scene and agent generation. */
    struct AiAgentInspectionSnapshot final {
        static constexpr std::size_t MaximumDecisionTasks = 32;
        AgentHandle agent;
        Runtime::EntityRef owner;
        std::uint64_t sceneRevision{};
        std::uint64_t decisionTick{}; /**< Zero until this Scene's scheduler publishes a completed report. */
        BlackboardInstanceBinding blackboard;
        std::uint64_t blackboardRevision{};
        std::vector<AiInspectionBlackboardRow> keys;
        std::optional<AiInspectionTask> task;
        std::vector<DecisionActiveTask> decisionTasks;
        std::size_t omittedDecisionTasks{};
        std::optional<AiInspectionFailure> decisionFailure;
        std::vector<AiInspectionStimulus> stimuli;
        std::optional<AiInspectionFailure> perceptionFailure;
        std::vector<AiInspectionSenseProvider> senseProviders;
        std::size_t perceptionProjectionDrops{}; /**< Capacity drops from the last synchronized projection. */
    };

    /** @brief Detached evidence from one selected decision and the canonical AI population at a completed tick. */
    struct AiSceneDecisionReplayCapture final {
        Runtime::SceneRuntimeId scene;
        std::uint64_t sceneStructureRevision{};
        std::uint64_t tick{};
        AgentId subject;
        BlackboardInstanceBinding blackboard; /**< Exact frozen observation consumed by the selected decision. */
        std::uint64_t blackboardRevision{};
        std::shared_ptr<const DecisionStepTrace> trace;
        AiCanonicalState canonical;

        /** @brief Borrows the existing v4 trace for comparison while this detached capture stays alive. */
        [[nodiscard]] AiReplayDecisionObservation Decision() const & noexcept {
            return {subject, blackboard, blackboardRevision, trace.get()};
        }

        AiReplayDecisionObservation Decision() const && = delete;
    };

    /** @brief Immutable value snapshot of one AI scene publication. */
    class AiSceneSnapshot final {
    public:
        /** @brief Reports whether this snapshot is bound to a scene incarnation. @return True for a valid snapshot. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return binding_.IsValid();
        }

        /** @brief Returns the exact activation binding. @return Immutable AI/Scene identity pair. */
        [[nodiscard]] constexpr const AiSceneActivationBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns the retained agent records in stable slot order. @return Borrowed record span. */
        [[nodiscard]] constexpr std::span<const AiAgentRuntimeRecord> Agents() const noexcept {
            return agents_;
        }

        /**
         * @brief Resolves one exact agent handle against this publication.
         * @param handle Generation-fenced agent handle.
         * @return Matching record or AIErrors::HandleInvalid.
         */
        [[nodiscard]] Result<AiAgentRuntimeRecord> Find(AgentHandle handle) const;

    private:
        friend class AiSceneRuntime;
        AiSceneActivationBinding binding_;
        std::vector<AiAgentRuntimeRecord> agents_;
        std::vector<std::optional<std::size_t>> slotLookup_; /**< Snapshot slot index to compact record index. */
    };

    namespace Detail {
        struct AiSceneRuntimeState;
        struct SceneDecisionReplayPending;
        struct SceneRestoreInput;
        struct PerceptionGroupPublication;
        struct AgentRuntimeState;
        class AiSceneStructuralParticipant;
        struct AiSceneRestoreState;
    }  // namespace Detail

    /** @brief Detached complete restore transaction; destruction rolls back without touching live or authored state. */
    class AiSceneRestoreCandidate final {
    public:
        /** @brief Runtime-only construction token; consumers cannot manufacture a candidate. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class AiSceneRuntime;
        };

        /**
         * @brief Takes ownership of runtime-validated detached restore state.
         * @param key Unforgeable token issued only by the runtime preparation boundary.
         * @param state Complete detached restore state owned until commit or rollback.
         */
        AiSceneRestoreCandidate(const ConstructionKey &key, std::unique_ptr<Detail::AiSceneRestoreState> state) noexcept;
        ~AiSceneRestoreCandidate();
        AiSceneRestoreCandidate(const AiSceneRestoreCandidate &) = delete;
        AiSceneRestoreCandidate &operator=(const AiSceneRestoreCandidate &) = delete;

    private:
        friend class AiSceneRuntime;
        std::unique_ptr<Detail::AiSceneRestoreState> state_;
    };

    /**
     * @brief Detached, transactional AI scene state prepared for aggregate RuntimeScene publication.
     * @details The candidate owns every blackboard and lifecycle object until publication or rollback. Publish is no-fail.
     */
    class AiSceneActivationCandidate final : public Runtime::SceneActivationCandidate {
    public:
        ~AiSceneActivationCandidate() override;

        /**
         * @brief Takes ownership of detached runtime state prepared for scene publication.
         * @param runtime Runtime owner that will publish or roll back the state.
         * @param binding Exact AI/Scene identity pair associated with the state.
         * @param state Detached state owned until publication or shutdown.
         * @param publicationToken Monotonic token fencing this candidate's publication.
         */
        AiSceneActivationCandidate(AiSceneRuntime &runtime, AiSceneActivationBinding binding,
                                   std::shared_ptr<Detail::AiSceneRuntimeState> state, std::uint64_t publicationToken) noexcept;

        AiSceneActivationCandidate(const AiSceneActivationCandidate &) = delete;
        AiSceneActivationCandidate &operator=(const AiSceneActivationCandidate &) = delete;

        /** @copydoc Runtime::SceneActivationCandidate::ValidatePublication */
        [[nodiscard]] Result<void> ValidatePublication() const override;

        /** @copydoc Runtime::SceneActivationCandidate::CanonicalDatasetProjection */
        [[nodiscard]] Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
            return Runtime::SceneCanonicalDatasetProjection::Absent;
        }

        /** @copydoc Runtime::SceneActivationCandidate::Publish */
        void Publish() noexcept override;
        /** @copydoc Runtime::SceneActivationCandidate::Shutdown */
        void Shutdown() noexcept override;

        /** @brief Returns the exact AI/Scene binding staged by this candidate. @return Immutable binding. */
        [[nodiscard]] constexpr const AiSceneActivationBinding &Binding() const noexcept {
            return binding_;
        }

    private:
        friend class AiSceneActivationParticipant;
        friend class AiSceneRuntime;
        /** @brief Stages exact spatial authority while the whole Scene aggregate is still private. */
        [[nodiscard]] Result<void> PreparePerception(const Runtime::RuntimeSceneView &scene, const AiScenePerceptionActivationInput &input);

        AiSceneRuntime *runtime_{};
        AiSceneActivationBinding binding_;
        std::unique_ptr<Detail::PerceptionGroupPublication> perception_;
        std::shared_ptr<Detail::AiSceneRuntimeState> state_;
        std::optional<Runtime::RuntimeSceneView> restoredScene_;
        CancellationToken restoreCancellation_;
        std::uint64_t publicationToken_{};
        bool published_{};
    };

    /** @brief Explicit product policy for a well-formed controller whose descriptor is unavailable. */
    enum class AiMissingControllerPolicy : std::uint8_t {
        Reject,
        PreserveInactive,
        Count,
    };

    /** @brief Bounded host policy and staged capability evidence for one AI scene runtime. */
    struct AiSceneRuntimeSettings final {
        std::size_t maximumAgents{MaximumAiSceneAgents};
        AiCapabilitySet availableCapabilities{AiCapabilitySet::Of(AiCapability::Behavior)
                                                  .Union(AiCapabilitySet::Of(AiCapability::Navigation))
                                                  .Union(AiCapabilitySet::Of(AiCapability::Perception))};
        AiMissingControllerPolicy missingController{AiMissingControllerPolicy::Reject};
        bool requirePerceptionEndpoints{}; /**< Product Scene hosts require explicitly admitted endpoint metadata. */

        /** @brief Checks product limits and closed capability bits. @return True when the settings are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumAgents > 0 && maximumAgents <= MaximumAiSceneAgents && availableCapabilities.IsValid() &&
                   missingController < AiMissingControllerPolicy::Count;
        }
    };

    /** @brief Scene-owned admission port for intents staged by cooked decision providers. */
    class IAiCookedDecisionIntentSink {
    public:
        virtual ~IAiCookedDecisionIntentSink() = default;
        /** @brief Publishes the exact agent's staged requests within the scheduler's command budget.
         * @param context Validated dispatch identity. @param budget Bounded command allowance.
         * @return True when all work is committed, false when work remains, or a typed dispatch failure.
         */
        [[nodiscard]] virtual Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept = 0;
        /** @brief Discards unpublished requests for a cancelled or replaced decision. @param agent Exact decision agent. */
        virtual void Discard(AgentHandle agent) noexcept = 0;
    };

    /** @brief Timing contract for external outcomes consumed by cooked decisions. */
    enum class AiCookedDecisionTiming : std::uint8_t {
        Deterministic,
        BestEffortExternalFeedback,
        BestEffortReactive,
        BestEffortQueries
    };

    /** @brief Immutable host selection of signed integer Blackboard inputs in model feature order. */
    struct AiSceneLocalPolicyBinding final {
        std::shared_ptr<const LocalLearnedPolicy> model;
        AiCapabilityDescriptorSet tasks;
        std::vector<BlackboardKeyId> featureKeys;
    };

    using AiSceneLocalPolicyCatalog = std::map<ControllerTypeId, std::shared_ptr<const AiSceneLocalPolicyBinding>>;

    /** @brief Opt-in fixed-tick execution for budgeted cooked decisions owned by this Scene. */
    struct AiSceneCookedDecisionExecution final {
        JobSystem &jobs; /**< Process executor must outlive the scene scheduler and its worker image pins. */
        AiTaskSchedulerSettings scheduler;
        AiAgentSchedulePolicy agent; /**< Maximum per-agent allowances; compiled plans reserve only required work units. */
        std::shared_ptr<IDecisionQueryTaskBinder> queryTasks; /**< Explicit Scene owner binding for authored query tasks. */
        std::shared_ptr<IAiCookedDecisionIntentSink> intents; /**< Explicit owner for deferred decision requests. */
        AiCookedDecisionTiming timing{AiCookedDecisionTiming::Deterministic};
        std::shared_ptr<const AiSceneLocalPolicyCatalog> localPolicies;

        /** @brief Checks that scheduler semantics match the declared feedback authority. */
        [[nodiscard]] bool HasMatchingTiming() const noexcept {
            if (timing == AiCookedDecisionTiming::Deterministic)
                return scheduler.mode == AiSchedulingMode::Deterministic;
            return scheduler.mode == AiSchedulingMode::BestEffort &&
                   (timing == AiCookedDecisionTiming::BestEffortReactive ||
                    (timing == AiCookedDecisionTiming::BestEffortQueries && queryTasks) ||
                    (timing == AiCookedDecisionTiming::BestEffortExternalFeedback && intents));
        }
    };

    /**
     * @brief Owner-thread AI runtime bound to one exact RuntimeScene publication.
     * @details All active state is detached before publication. Handles include an AI incarnation and never outlive replacement,
     * destruction, or shutdown. Disable retires owned task work before clearing blackboard and capability state.
     */
    class AiSceneRuntime final {
    public:
        /** @brief Creates an empty bounded AI scene runtime.
         * @param settings Host capability and capacity policy. @return Runtime or typed policy error.
         */
        [[nodiscard]] static Result<AiSceneRuntime> Create(AiSceneRuntimeSettings settings = {});

        /** @brief Creates an empty runtime with opted-in transactional cooked decision execution.
         * @param settings Host capability and capacity policy.
         * @param decisionExecution Deterministic scheduler policy and longer-lived process jobs.
         * @return Runtime or typed policy error.
         */
        [[nodiscard]] static Result<AiSceneRuntime> Create(AiSceneRuntimeSettings settings,
                                                           AiSceneCookedDecisionExecution decisionExecution);

        /** @brief Issues weak access to this exact publication on its simulation safe-point thread.
         * @return Empty/unavailable access when inactive; replacement permanently invalidates retained access. */
        [[nodiscard]] AiRuntimeCapability CapabilityAtSafePoint() const;

        AiSceneRuntime(const AiSceneRuntime &) = delete;
        AiSceneRuntime &operator=(const AiSceneRuntime &) = delete;
        AiSceneRuntime(AiSceneRuntime &&) noexcept;
        AiSceneRuntime &operator=(AiSceneRuntime &&) = delete;
        ~AiSceneRuntime();

        /**
         * @brief Prepares the complete AI population for one RuntimeScene activation.
         * @param binding Exact AI and RuntimeScene incarnation pair.
         * @param agents Typed entity-owned component values.
         * @param descriptors Immutable controller descriptor catalog.
         * @return Detached candidate; active state is unchanged on any failure.
         */
        [[nodiscard]] Result<std::unique_ptr<AiSceneActivationCandidate>> PrepareScene(AiSceneActivationBinding binding,
                                                                                       std::span<const AiSceneAgentDescriptor> agents,
                                                                                       std::span<const AiControllerDescriptor> descriptors);

        /**
         * @brief Prepares the entire AI population against an unpublished, fully reconstructed Scene.
         * @param scene Exact destination view after the aggregate restores its entity population.
         * @param descriptors Host-admitted controller catalog.
         * @param state Complete decoded canonical AI population.
         * @param migrations Explicit direct schema migrations borrowed during preparation.
         * @param cancellation Aggregate restore cancellation.
         * @return Detached activation candidate, or a typed failure with the live population intact.
         * @pre Called by the owner lane. The destination Scene outlives the candidate.
         */
        [[nodiscard]] Result<std::unique_ptr<AiSceneActivationCandidate>> PrepareRestoredScene(
            const Runtime::RuntimeSceneView &scene, std::span<const AiControllerDescriptor> descriptors, const AiCanonicalState &state,
            std::span<const BlackboardSchemaMigration> migrations = {}, CancellationToken cancellation = {});

        /**
         * @brief Captures the only canonical AI population at the aggregate snapshot safe point.
         * @param scene Current Scene borrow used to revalidate every owner generation and entity-valued key.
         * @return Complete stable-identity-sorted state or typed lifecycle/storage failure.
         * @pre Called on the simulation owner; the Scene remains alive for the call.
         */
        [[nodiscard]] Result<AiCanonicalState> CaptureCanonicalState(Runtime::RuntimeSceneView scene) const;
        /**
         * @brief Captures canonical AI with a host-admitted limit on detached population storage.
         * @param scene Current Scene borrow.
         * @param maximumBytes Maximum requested storage for owned agent and Blackboard entry elements.
         * @return Complete population or a typed limit, lifecycle, or storage failure.
         * @pre Called on the simulation owner; the Scene remains alive for the call.
         */
        [[nodiscard]] Result<AiCanonicalState> CaptureCanonicalState(Runtime::RuntimeSceneView scene, std::size_t maximumBytes) const;
        /**
         * @brief Validates and stages a complete population restore without changing live or authored state.
         * @param scene Current destination Scene borrow; its owner must outlive staging through commit or rollback.
         * @param expectedBinding Exact caller-selected destination AI/Scene incarnation.
         * @param state Complete canonical source population matching the destination authored agents/controllers.
         * @param migrations Bounded inert direct schema migration catalog, borrowed only during preparation.
         * @param cancellation Cooperative cancellation serviced during staging and again before publication.
         * @return Detached owned transaction or typed malformed, stale, unsupported, migration, or cancellation diagnostic.
         * @pre Simulation owner thread; restore is load-time work and may allocate bounded staging storage.
         */
        [[nodiscard]] Result<std::unique_ptr<AiSceneRestoreCandidate>> PrepareRestoreAtSafePoint(
            Runtime::RuntimeSceneView scene, AiSceneActivationBinding expectedBinding, const AiCanonicalState &state,
            std::span<const BlackboardSchemaMigration> migrations = {}, CancellationToken cancellation = {}) const;
        /**
         * @brief Revalidates all destination fences then atomically restores canonical state and retires transient work.
         * @param candidate Transaction prepared by this runtime; consumed on success or failure.
         * @return Success or typed stale/cancelled failure with all prior state and work preserved.
         * @pre Simulation owner at the aggregate restore safe point; the borrowed Scene is still alive.
         * @post Success cancels old tasks and observers before releasing their storage; asynchronous work restarts separately.
         */
        [[nodiscard]] Result<void> CommitRestoreAtSafePoint(std::unique_ptr<AiSceneRestoreCandidate> candidate);

        /** @brief Binds the now-callable generation to a privately staged Scene and its exact spatial owner.
         * @param scene Current private Scene view. @param spatial The Scene's already admitted endpoint owner.
         * @param execution Selected native generation and explicit host policy.
         * @return Success or typed refusal with no installed sensing instances after failure.
         */
        [[nodiscard]] Result<void> ActivatePerceptionAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                 const PerceptionSpatialBroadphase &spatial,
                                                                 const AiScenePerceptionExecution &execution);

        /** @brief Advances admitted native senses on the exact committed owner lane.
         * @param scene Current Scene and endpoint lifetime fence. @param spatial Current Scene's admitted spatial owner.
         * @param tick Authoritative simulation tick. @param fixedStep Exact host quantum.
         * @return Success or a Scene/policy failure. Repeated calls never replay the same tick.
         * @details A provider fault in a current Scene suspends that recipient's sensing until its activation is replaced.
         * Other recipients continue within their admitted budgets.
         */
        [[nodiscard]] Result<void> PollPerceptionAtSensePoll(const Runtime::RuntimeSceneView &scene,
                                                             const PerceptionSpatialBroadphase &spatial, std::uint64_t tick,
                                                             Duration fixedStep);

        /** @brief Reads owned memory for one exact existing recipient, filtering stale subjects and emitters.
         * @param scene Current Scene publication. @param recipient Exact recipient entity generation.
         * @return Bounded snapshot, the original provider error for a suspended recipient, or a typed unavailable/stale error.
         */
        [[nodiscard]] Result<PerceptionMemorySnapshot> ReadPerceptionAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                 Runtime::EntityRef recipient);

        /** @brief Captures bounded, currently authorized memory for one exact decision agent.
         * @param scene Current Scene on the owner lane. @param agent Exact handle of the requesting controller.
         * @return Detached observations and revocable provider/filter authority, or a typed admission failure.
         */
        [[nodiscard]] Result<PerceptionQueryInput> CaptureQueryPerceptionAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                     AgentHandle agent) const;

        /** @brief Activates cooked decisions once on an exact already-staged Scene before host publication.
         * @param execution Deterministic owner policy with live process jobs and callable provider generation.
         * @return Success or typed failure preserving the current scheduler, agents and task slots.
         * @details For composed hosts whose native providers become callable only after exact platform scopes exist.
         * Call while the initial Scene is still private to the host, before entering its first simulation tick.
         */
        [[nodiscard]] Result<void> ActivateCookedDecisionsAtSafePoint(AiSceneCookedDecisionExecution execution);

        /** @brief Replaces one budgeted cooked decision at the owner's completed tick boundary.
         * @param agent Exact active Scene agent generation.
         * @param previous Exact currently published cooked plan and image owner.
         * @param replacement Current compiled revision of the same root.
         * @param migration Optional direct forward migration for a newer version of the same authored Blackboard schema.
         * @return Success or a typed rejection preserving the prior decision, Blackboard and task range.
         * @details Executable-equivalent closures retain active call frames, tasks and callbacks. An accepted restart
         * retires active nodes with PlanReplaced and restarts the root. A schema migration stages a new Blackboard and revokes
         * the previous read, write and task reservation leases at publication. Agents with separate live task or sensing
         * authorities require coordinated Scene replacement for a schema migration. All other accepted closures restart as a unit.
         * @pre Actual Scene owner lane, before the first evaluation or after the current intent dispatch. */
        [[nodiscard]] Result<void> ReplaceCookedDecisionAtSafePoint(AgentHandle agent, std::shared_ptr<const CookedDecisionPlan> previous,
                                                                    std::shared_ptr<const CookedDecisionPlan> replacement,
                                                                    const BlackboardSchemaMigration *migration = nullptr);

        /** @brief Runs one admitted query task's BlackboardSync against its exact Scene-owned destination.
         * @param task Existing owner-bound query task. @param scene Current view for output revalidation.
         * @param tick Exact simulation tick before that agent's next decision.
         * @param publication Optional detached observer of a result published on this tick.
         * @return True if an output was committed, false for idle or pending work, or a typed stale or publication error.
         */
        [[nodiscard]] Result<bool> SyncDecisionQueryAtBlackboardSync(DecisionQueryTask &task, const Runtime::RuntimeSceneView &scene,
                                                                     std::uint64_t tick,
                                                                     std::optional<DecisionQueryPublication> *publication = nullptr) const;
        /** @brief Resolves the exact live Scene entity for owner-side query context capture.
         * @param agent Current AI agent generation. @return Exact entity reference or a typed stale-owner failure.
         */
        [[nodiscard]] Result<Runtime::EntityRef> ResolveDecisionQueryOwnerAtSafePoint(AgentHandle agent) const;

        /** @brief Composes the active scene's scheduler once, at the owner safe point.
         * @param jobs Process JobSystem, retained until every worker image pin drains.
         * @param settings Finite scene-wide scheduling limits. @return Success or typed unavailable/admission failure.
         * @details Scene replacement/restore closes this scheduler. Recompose and rebind executors after publication. */
        [[nodiscard]] Result<void> ConfigureTaskSchedulerAtSafePoint(JobSystem &jobs, const AiTaskSchedulerSettings &settings = {}) const;
        /** @brief Binds a decision only to an exact currently active scene agent.
         * @param agent Exact live handle. @param policy Finite frequency/work policy.
         * @param image Callback lifetime pin. @param executor Owned bounded owner evaluator/intent committer.
         * @return Success or typed stale/disabled/unconfigured/admission failure. */
        [[nodiscard]] Result<void> RegisterDecisionAtSafePoint(AgentHandle agent, const AiAgentSchedulePolicy &policy,
                                                               std::shared_ptr<const void> image,
                                                               std::shared_ptr<IAiScheduledDecision> executor) const;
        /** @brief Borrows scheduling at its declared owner phases.
         * @return Active scheduler or typed unavailable failure.
         * @details Borrow expires on scene replacement, restore, shutdown or destruction. Workers must never retain it. */
        [[nodiscard]] Result<AiTaskScheduler *> TaskSchedulerAtSafePoint() const;
        /** @brief Installs event-only cooked node dependencies on the exact agent's admitted native Perception listeners.
         * An empty span clears the subscription. Validation and allocation complete before the current subscription is changed.
         */
        [[nodiscard]] Result<void> SetCookedPerceptionWatchesAtSafePoint(
            AgentHandle agent, std::span<const DecisionWakePerceptionDependency> dependencies) const;

        /** @brief Copies the first fatal failure or, if absent, first provider advisory from the last completed tick.
         * @param scene The current Scene view owned by this AI runtime.
         * @return A copied failure, empty after a clean tick, or a typed error when the Scene or scheduler is unavailable.
         */
        [[nodiscard]] Result<std::optional<AiRuntimeDecisionDiagnostic>> ReadDecisionFailureAtSafePoint(
            const Runtime::RuntimeSceneView &scene) const;

        /**
         * @brief Starts one owned task at the AI decision safe point.
         * @param agent Exact active agent handle.
         * @param taskDefinition Persistent task identity.
         * @return Generation-fenced task handle or a typed lifecycle/capacity error.
         */
        [[nodiscard]] Result<TaskHandle> StartTaskAtSafePoint(AgentHandle agent, TaskId taskDefinition) const;

        /** @brief Reserves a disjoint logical task range from the same high-water used by native/script tasks.
         * @param agent Exact active agent with a live blackboard. @param count Required exclusive slot count; zero is valid.
         * @return Move-only reservation, or typed owner/agent/capacity/storage failure without consuming indices.
         * @details Owner-thread activation/reload work; no task or provider is started. Successful indices are never reclaimed
         * in this Scene incarnation, including after failed graph creation. Destruction/agent retirement cancels the scope.
         */
        [[nodiscard]] Result<AiDecisionTaskReservation> ReserveDecisionTaskSlotsAtSafePoint(AgentHandle agent, std::uint32_t count) const;
        /** @brief Captures the actual agent blackboard for one frozen decision step.
         * @param agent Exact active agent. @return Owned immutable snapshot or typed unavailable/stale/storage failure.
         * @details Call on the simulation owner after BlackboardSync; no mutable blackboard or Scene pointer escapes.
         */
        [[nodiscard]] Result<BlackboardSnapshot> CaptureDecisionSnapshotAtSafePoint(AgentHandle agent) const;

        /** @brief Copies read-only inspection data for one exact live Scene agent on the simulation owner.
         * @param scene Current committed Scene incarnation and structural revision.
         * @param agent Exact selected agent generation.
         * @param disclosedKeys Explicit host-approved Blackboard key identities; an empty list redacts all values.
         * @return Detached bounded data or a typed selection, owner or storage failure.
         */
        [[nodiscard]] Result<AiAgentInspectionSnapshot> CaptureAgentInspectionAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                          AgentHandle agent,
                                                                                          std::span<const BlackboardKeyId> disclosedKeys);

        /** @brief Reserves evidence for one selected cooked decision before its next scheduled tick.
         * @param agent Exact active Scene agent generation selected by the host.
         * @param tick Next fixed tick, or the initial positive tick before scheduling starts.
         * @return Success or typed owner, phase, capacity or plan failure.
         * @details Owner-only, with one bounded buffer. A new successful arm replaces any unconsumed request.
         * Scheduler budgets, callbacks and evaluation order are unchanged. */
        [[nodiscard]] Result<void> ArmDecisionReplayAtSafePoint(AgentHandle agent, std::uint64_t tick) const;

        /** @brief Arms the same bounded evidence for inspection, including decisions backed by a local policy model. */
        [[nodiscard]] Result<void> ArmDecisionTraceAtSafePoint(AgentHandle agent, std::uint64_t tick) const;

        /** @brief Drops an unconsumed replay arm or observation on the simulation owner.
         * @param agent Exact agent generation originally armed.
         * @param tick Exact fixed tick originally armed.
         * @details A mismatched request has no effect. This releases only recording storage and preserves
         * decisions, scheduler state and detached evidence. */
        void DiscardDecisionReplayAtSafePoint(AgentHandle agent, std::uint64_t tick) const noexcept;

        /** @brief Detaches the exact selected agent's bounded trace after scheduler commit.
         * @param scene Current Scene on its owner lane. @param agent Exact armed agent generation.
         * @param tick Exact completed Decision tick.
         * @return Immutable trace including its overflow count, or a typed stale/phase error.
         * @details Consumes the arm only on success and does not capture canonical agent state.
         */
        [[nodiscard]] Result<std::shared_ptr<const DecisionStepTrace>> TakeDecisionTraceAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                                    AgentHandle agent,
                                                                                                    std::uint64_t tick) const;

        /** @brief Detaches the actual armed decision trace and canonical base-agent population after committed simulation.
         * @param scene Current committed Scene after the host completed its fixed tick.
         * @param tick Exact armed tick with completed AI intent dispatch.
         * @param maximumCanonicalBytes Caller-owned canonical capture allocation budget.
         * @return Owned evidence or typed stale, incomplete or capacity failure. Only success consumes the request.
         * @details No hashing, serialization or repeated decision evaluation happens here. The host may transfer the detached result
         * to a worker for hashing and pass its Decision() observation to replay comparison. */
        [[nodiscard]] Result<AiSceneDecisionReplayCapture> CaptureDecisionReplayAtSafePoint(Runtime::RuntimeSceneView scene,
                                                                                            std::uint64_t tick,
                                                                                            std::size_t maximumCanonicalBytes) const;

        /** @brief Publishes each authored projection once per sensed tick and current spatial revision before Decision.
         * @details The typed paired values share one Blackboard commit. An unchanged authorization renewal queues no wake.
         */
        [[nodiscard]] Result<void> PublishPerceptionBlackboardsAtBlackboardSync(const Runtime::RuntimeSceneView &scene,
                                                                                const PerceptionSpatialBroadphase &spatial,
                                                                                std::uint64_t tick);

        /** @brief Publishes the currently registered Perception endpoints through the canonical spatial owner.
         * @param scene Exact committed Scene. @param revision Positive owner-issued spatial revision.
         * @param positions Exact host-captured global positions in ascending entity order.
         * @param spatial Borrowed owner. @param filtering Complete authoritative Gameplay filter policy.
         * @return One complete immutable publication or a typed failure preserving the previous spatial publication.
         */
        [[nodiscard]] Result<std::shared_ptr<const PerceptionSpatialSnapshot>> PublishPerceptionAtSafePoint(
            const Runtime::RuntimeSceneView &scene, std::uint64_t revision, std::span<const AiScenePerceptionPosition> positions,
            PerceptionSpatialBroadphase &spatial, const PerceptionFilteringPolicy &filtering = {}) const;

        /**
         * @brief Disables one agent, cancelling its task before revoking its capabilities and blackboard.
         * @param agent Exact active agent handle.
         * @return Success or a typed malformed/stale/shutdown error.
         */
        [[nodiscard]] Result<void> DisableAtSafePoint(AgentHandle agent) const;

        /**
         * @brief Retires the agent owned by one destroyed runtime entity generation.
         * @param owner Exact RuntimeScene entity reference.
         * @return Number retired or a typed malformed/stale/shutdown error.
         */
        [[nodiscard]] Result<std::size_t> RetireOwnerAtSafePoint(Runtime::EntityRef owner) const;

        /** @brief Captures an immutable value snapshot of the active publication. */
        [[nodiscard]] Result<AiSceneSnapshot> Snapshot() const;
        /** @brief Resolves one exact handle against the active publication. */
        [[nodiscard]] Result<AiAgentRuntimeRecord> Find(AgentHandle agent) const;
        /** @brief Returns the exact active AI/Scene binding. */
        [[nodiscard]] Result<AiSceneActivationBinding> ActiveBinding() const;

        /** @brief Cancels owned work and closes admission; retained external snapshots remain valid. */
        void BeginShutdown() noexcept;

        /** @brief Creates an explicit owner-lane adapter for atomic entity-group additions and retirement.
         * @param descriptors Immutable controller catalog copied into the owned adapter.
         * @param perception Optional host-owned atomic endpoint publication.
         * @return Adapter borrowing this runtime, which must outlive the Scene service using it.
         * @details Preparation never replaces existing agents/tasks. Detached candidates retain their publication
         * across shutdown and reject publication after any owner mutation. Public C++ consumers must rebuild.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant(
            std::span<const AiControllerDescriptor> descriptors, std::optional<AiScenePerceptionGroupInput> perception = std::nullopt);

        /** @brief Reports whether the runtime rejects new activation and safe-point operations. */
        [[nodiscard]] constexpr bool IsShutdown() const noexcept {
            return shutdown_;
        }

    private:
        friend class AiSceneActivationCandidate;
        friend class Detail::AiSceneStructuralParticipant;

        [[nodiscard]] Result<void> ArmCookedDecisionEvidence(AgentHandle agent, std::uint64_t tick, bool replay) const;

        /** @brief Validates the one exact observed trace without consuming it. */
        [[nodiscard]] Result<Detail::SceneDecisionReplayPending *> FindDecisionTraceAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                                                std::uint64_t tick) const;

        /** @brief Reads the exact selected cooked executor on the established Scene safe point. */
        [[nodiscard]] std::size_t CopyCookedDecisionTasks(AgentHandle agent, const std::shared_ptr<const CookedDecisionPlan> &plan,
                                                          std::span<DecisionActiveTask> output) const;

        explicit AiSceneRuntime(AiSceneRuntimeSettings settings, std::optional<AiSceneCookedDecisionExecution> execution) noexcept;

        /** @brief Stages one exact Scene-owned execution and exclusive task interval without publishing the cursor. */
        [[nodiscard]] Result<AiScheduledDecisionRegistration> PrepareCookedDecisionRegistration(
            const std::shared_ptr<Detail::AiSceneRuntimeState> &state, const Detail::AgentRuntimeState &agent,
            const std::shared_ptr<const CookedDecisionPlan> &plan, std::uint32_t &nextTaskSlot) const;

        /** @brief Stages an admitted restart and publishes its Blackboard and scheduler image together. */
        [[nodiscard]] Result<void> RestartCookedDecisionAtSafePoint(const std::shared_ptr<Detail::AiSceneRuntimeState> &state,
                                                                    Detail::AgentRuntimeState &slot,
                                                                    std::shared_ptr<const CookedDecisionPlan> replacement,
                                                                    Detail::AgentRuntimeState *migrated);

        /** @brief Prepares decisions and exact task ranges in detached storage for a scene or structural transaction. */
        [[nodiscard]] Result<std::vector<AiScheduledDecisionRegistration>> PrepareCookedDecisionRegistrations(
            const std::shared_ptr<Detail::AiSceneRuntimeState> &state, std::span<const Detail::AgentRuntimeState> agents,
            std::uint32_t &nextTaskSlot) const;
        /** @brief Composes the opt-in scheduler on the entirely detached initial population. */
        [[nodiscard]] Result<void> PrepareCookedDecisionScheduler(const std::shared_ptr<Detail::AiSceneRuntimeState> &state) const;

        /** @brief Builds detached state and applies optional canonical values before constructing its scheduler. */
        [[nodiscard]] Result<std::unique_ptr<AiSceneActivationCandidate>> PrepareSceneInternal(
            AiSceneActivationBinding binding, std::span<const AiSceneAgentDescriptor> agents,
            std::span<const AiControllerDescriptor> descriptors, const Detail::SceneRestoreInput *restore);

        [[nodiscard]] Result<void> ValidateCandidate(const AiSceneActivationBinding &binding) const;
        [[nodiscard]] bool PublishCandidate(std::shared_ptr<Detail::AiSceneRuntimeState> &state, const AiSceneActivationBinding &binding,
                                            std::uint64_t publicationToken) noexcept;
        void ShutdownState(Detail::AiSceneRuntimeState &state) const noexcept;
        void RetirePublication(std::uint64_t publicationToken) noexcept;

        AiSceneRuntimeSettings settings_;
        std::optional<AiSceneCookedDecisionExecution> decisionExecution_;
        std::shared_ptr<Detail::AiSceneRuntimeState> active_;
        std::uint64_t nextPublicationToken_{1};
        std::uint64_t activePublicationToken_{};
        bool shutdown_{};
    };

    /** @brief Decoded values and cancellation for one exact reconstructed Scene. */
    struct AiSceneRestoredPopulation final {
        std::shared_ptr<const AiCanonicalState> state; /**< Empty selects ordinary authored activation. */
        CancellationToken cancellation;
    };

    /** @brief Host-owned selection of saved values already reconciled with candidate content. */
    using AiSceneRestoreSource = std::function<Result<AiSceneRestoredPopulation>(const Runtime::RuntimeSceneView &)>;

    /**
     * @brief RuntimeScene activation participant for typed AI agent/controller components.
     * @details It resolves entity generations before publication and never mutates the authoring document.
     */
    class AiSceneActivationParticipant final : public Runtime::SceneActivationParticipant {
    public:
        /**
         * @brief Binds a scene runtime and inert descriptor catalog whose lifetimes exceed this participant.
         * @param runtime Owner-thread AI scene runtime.
         * @param descriptors Immutable controller dependency descriptors.
         * @param perception Explicit optional spatial policy; its publisher outlives the Scene activation and structural adapters.
         * @param restore Optional owner-lane source of decoded state for the exact candidate Scene.
         */
        AiSceneActivationParticipant(AiSceneRuntime &runtime, std::span<const AiControllerDescriptor> descriptors,
                                     std::optional<AiScenePerceptionActivationInput> perception = {},
                                     AiSceneRestoreSource restore = {}) noexcept
            : runtime_(&runtime), descriptors_(descriptors), perception_(std::move(perception)), restore_(std::move(restore)) {}

        /** @copydoc Runtime::SceneActivationParticipant::Prepare */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         const Runtime::RuntimeSceneView &scene) override;

    private:
        /** @brief Projects the authored source for normal Scene startup. */
        [[nodiscard]] Result<std::unique_ptr<AiSceneActivationCandidate>> PrepareAuthoredScene(
            const Runtime::RuntimeSceneDefinition &definition, const Runtime::RuntimeSceneView &scene);

        AiSceneRuntime *runtime_{};
        std::span<const AiControllerDescriptor> descriptors_;
        std::optional<AiScenePerceptionActivationInput> perception_;
        AiSceneRestoreSource restore_;
    };
}  // namespace Horo::AI
