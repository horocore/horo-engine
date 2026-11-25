#pragma once

/** @file AITaskScheduler.h
 * @brief Scene-owned fixed-tick admission and cooperative slicing over the Foundation JobSystem.
 */
#include "Horo/AI/AITaskJobService.h"
#include "Horo/AI/DecisionControl.h"
#include "Horo/AI/DecisionWakePolicy.h"

#include <span>

namespace Horo::AI {
    class CookedDecisionPlan;

    namespace Detail {
        struct AiSceneRuntimeState;
    }

    /** @brief Authored priority; starvation promotion precedes ordinary priority. */
    enum class AiTaskPriority : std::uint8_t {
        Background,
        Normal,
        Urgent,
        Count
    };
    /** @brief Explicit result of one cooperative decision slice. */
    enum class AiSliceOutcome : std::uint8_t {
        Complete,
        Yielded,
        BudgetExhausted,
        Count
    };
    /** @brief Deterministic full-rate owner admission versus best-effort bounded time slicing. */
    enum class AiSchedulingMode : std::uint8_t {
        Deterministic,
        BestEffort,
        Count
    };

    /** @brief Owner scheduling tier, ordered from most active to least active. */
    enum class AiSimulationTier : std::uint8_t {
        Full,
        Reduced,
        EventOnly,
        Suspended,
    };

    /** @brief Host-authored best-effort distance bands and temporal hysteresis. */
    struct AiDecisionSimulationProfile final {
        std::uint64_t fullDistanceMillimeters{};       /**< Inclusive full cadence limit. */
        std::uint64_t reducedDistanceMillimeters{};    /**< Inclusive reduced cadence limit. */
        std::uint64_t suspensionDistanceMillimeters{}; /**< Beyond this band, unrelated agents may suspend. */
        std::uint64_t reducedIntervalTicks{};          /**< Minimum positive reduced cadence, greater than one. */
        std::uint64_t demotionDelayTicks{};            /**< Positive ticks for which the candidate tier must persist. */
    };

    /** @brief Immutable gameplay-owned relevance facts. View/frustum state is outside this contract. */
    struct AiDecisionRelevance final {
        std::optional<std::uint64_t> anchorDistanceMillimeters; /**< Distance to the nearest simulation anchor; absence preserves Full. */
        bool activeSimulationRegion{}; /**< Gameplay or streaming relevance keeps distant agents at least event-driven. */
        bool gameplayPinned{};         /**< Gameplay-essential work remains Full. */
        bool urgentStimulus{};         /**< High-priority committed gameplay stimulus promotes immediately. */
    };

    /** @brief One owner-observed relevance update for an exact Scene agent generation. */
    struct AiDecisionRelevanceUpdate final {
        AgentHandle agent;
        AiDecisionRelevance relevance;
    };

    /** @brief Non-copyable allowance; every node/query/command step must consume before doing work. */
    class AiWorkBudget final {
    public:
        /** @brief Creates a finite slice. @param units Maximum cooperative work steps. */
        explicit AiWorkBudget(std::size_t units) noexcept : remaining_(units) {}

        /** @brief Charges before work; failed charges do no work. @param units Positive cost. @return Whether admitted. */
        [[nodiscard]] bool Consume(std::size_t units = 1) noexcept;

        /** @brief Remaining admitted work. @return Finite unspent allowance. */
        [[nodiscard]] std::size_t Remaining() const noexcept {
            return remaining_;
        }

        /** @brief Whether any attempted step exceeded the allowance. @return Exhaustion evidence. */
        [[nodiscard]] bool Exhausted() const noexcept {
            return exhausted_;
        }

        AiWorkBudget(const AiWorkBudget &) = delete;
        AiWorkBudget &operator=(const AiWorkBudget &) = delete;

    private:
        std::size_t remaining_{};
        bool exhausted_{};
    };

    /** @brief Immutable bounded worker algorithm; detached inputs only, no owner executor or Scene capability. */
    class IAiBudgetedTaskJob : public IAiTaskJob {
    public:
        /** @brief Constructs finite cooperative worker allowance at composition. @param units Positive admitted work units. */
        explicit IAiBudgetedTaskJob(std::size_t units) noexcept : units_(units) {}

        /** @brief Returns declared finite cost. @return Worker slice work units. */
        [[nodiscard]] std::size_t WorkUnits() const noexcept {
            return units_;
        }

        /** @copydoc IAiTaskJob::Run */
        [[nodiscard]] Result<void> Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const final;
        /** @brief Runs immutable bounded work; consume before each step and observe cancellation between steps.
         * @param operation Exact detached task; check all its cancellation ancestors between bounded steps.
         * @param cancellation Also observe this Foundation worker ancestry. @param budget Finite work allowance.
         * @return Original typed result; an over-budget attempt becomes SchedulerBudgetExhausted. */
        [[nodiscard]] virtual Result<void> RunSlice(const AiTaskOperationContext &operation, const CancellationToken &cancellation,
                                                    AiWorkBudget &budget) const = 0;

    private:
        std::size_t units_{};
    };

    /** @brief Typed precomposed budgeted worker lease; admission copies pins without allocating a wrapper. */
    class AiWorkerSliceLease final {
    public:
        /** @brief Captures immutable bounded work and pins its code through destruction.
         * @param image Required callback/dependency pin. @param work Owned immutable slice algorithm.
         * @throws std::bad_alloc When paired lease storage cannot be allocated. */
        AiWorkerSliceLease(std::shared_ptr<const void> image, std::shared_ptr<const IAiBudgetedTaskJob> work)
            : units_(work ? work->WorkUnits() : 0), lease_(std::move(image), std::move(work)) {}

    private:
        friend class AiTaskScheduler;
        std::size_t units_{};
        AiTaskJobLease lease_;
    };

    /** @brief Exact owner execution identity and coalesced causes; no mutable Scene capability. */
    struct AiDecisionSliceContext final {
        AgentHandle agent;
        std::uint64_t tick{};
        DecisionWakeReasons reasons;
        CancellationToken cancellation;
        AiSimulationTier tier{AiSimulationTier::Full};
    };

    /**
     * @brief Host-bound owner evaluator and typed intent committer; never called on workers.
     * @details Retain resumable execution and bounded intent storage at registration. Evaluate reads frozen inputs,
     * consumes before each bounded step and never mutates live Scene/ECS. Commit consumes before each command at
     * AiIntentDispatch and queues structural changes through SceneCommandBuffer. Worker-safe computation uses
     * scheduler SubmitWorker with an immutable AiTaskJobLease, never this callback or a mutable Scene borrow.
     * Callbacks must obey cooperative budgets, retain no context/budget borrows and must not reenter scheduling.
     */
    class IAiScheduledDecision {
    public:
        virtual ~IAiScheduledDecision() = default;
        /** @brief Advances a resumable decision. @param context Frozen owner identity. @param budget Slice allowance.
         * @return Complete, yielded, exhausted, or original typed failure; failures discard this slice's intent batch. */
        [[nodiscard]] virtual Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept = 0;

        /** @brief Consumes the first raw provider failure retained by the exact evaluation, if supported.
         * @param tick Exact evaluated tick. @return Advisory evidence, independent of the slice outcome and intent lifetime.
         */
        [[nodiscard]] virtual std::optional<DecisionProviderFailure> TakeProviderFailure(std::uint64_t tick) noexcept {
            static_cast<void>(tick);
            return std::nullopt;
        }

        /** @brief Publishes a bounded prefix of evaluated intents. @param context Evaluation identity, revalidated by the scheduler.
         * @param budget Command allowance. @return True when all intents are committed, false to retain the bounded remainder. */
        [[nodiscard]] virtual Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept = 0;
        /** @brief Discards retained intents and transient evaluation at cancellation. Must be idempotent. */
        virtual void Cancel() noexcept = 0;

        /** @brief Retires the exact previous plan after a committed safe-point replacement. */
        virtual void CancelForReplacement() noexcept {
            Cancel();
        }

        /** @brief Checks current task readiness on the scheduler lane without executing providers or consuming payloads. */
        [[nodiscard]] virtual bool HasPendingTaskWake() const noexcept {
            return false;
        }

        /** @brief Reads an already cached fixed deadline without invoking any provider. */
        [[nodiscard]] virtual std::optional<std::uint64_t> NextFixedWakeTick() const noexcept {
            return std::nullopt;
        }

        /** @brief Borrows the immutable listener declarations admitted with this exact executable. */
        [[nodiscard]] virtual std::span<const PerceptionListenerTypeId> DeclaredPerceptionWatches() const noexcept {
            return {};
        }

        /** @brief Drops optional work while retaining a decision that can be evaluated after promotion.
         * @details Existing resumable executors may use their cancellation behavior. Executors with terminal
         * cancellation override this hook and retire only their transient callback generations.
         */
        virtual void SuspendForRelevance() noexcept {
            Cancel();
        }
    };

    /** @brief Lowerable per-agent frequency, work, command and worker-admission policy. */
    struct AiAgentSchedulePolicy final {
        AiTaskPriority priority{AiTaskPriority::Normal};
        std::uint64_t intervalTicks{1}; /**< Deterministic mode requires one. In best effort, zero disables periodic polling. */
        std::size_t workUnitsPerTick{64};
        std::size_t commandsPerTick{16};
        std::size_t workerSubmissionsPerTick{1};
        std::size_t pendingWorkers{1};
    };

    /** @brief Bounded scheduler inputs for one exact admitted agent generation. */
    struct AiScheduledDecisionRegistration final {
        AgentHandle agent;
        AgentId identity;
        AiAgentSchedulePolicy policy;
        std::shared_ptr<const void> image; /**< Pins executor code through destruction. */
        std::shared_ptr<IAiScheduledDecision> executor;
        CancellationToken cancellation;
    };

    /** @brief Finite scene-wide limits, independent of renderer, wall clock and worker count. */
    struct AiTaskSchedulerSettings final {
        static constexpr std::size_t HardAgents = 65'536;
        static constexpr std::size_t HardWorkUnits = 1'048'576;
        std::size_t maximumAgents{1'024};
        std::size_t evaluationsPerTick{128};
        std::size_t workUnitsPerTick{8'192};
        std::size_t commandsPerTick{2'048};
        std::size_t workerSubmissionsPerTick{128};
        std::size_t pendingWorkers{128};
        std::uint64_t starvationTicks{8};
        AiSchedulingMode mode{AiSchedulingMode::Deterministic};
        std::optional<AiDecisionSimulationProfile> simulationProfile; /**< Opt-in; accepted only in BestEffort mode. */
    };

    /** @brief Fixed phase in which a scheduled Decision callback failed. */
    enum class AiSchedulingFailurePhase : std::uint8_t {
        Evaluate,
        Commit
    };

    /** @brief Exact registered generation and phase preserved alongside the first error in a tick. */
    struct AiSchedulingFailureOrigin final {
        AgentHandle agent;
        AgentId identity;
        AiSchedulingFailurePhase phase{};
        DecisionGraphAssetId asset; /**< Present only when the failed callback supplied exact cooked source evidence. */
        DecisionNodeId node;
        bool advisory{}; /**< Provider error observed without failing the scheduler slice. */
    };

    /** @brief First non-fatal provider error captured alongside, independently of, any scheduler failure. */
    struct AiSchedulingAdvisoryFailure final {
        Error error;
        AiSchedulingFailureOrigin origin;
    };

    /** @brief Observable bounded outcomes for the most recent fixed tick; repeated calls never replenish it. */
    struct AiSchedulingReport final {
        std::uint64_t tick{};
        std::size_t evaluated{}, workUnits{}, commands{}, workerSubmissions{};
        std::size_t deferred{}, starved{}, exhausted{}, yielded{}, failed{}, cancelled{};
        std::optional<Error> firstFailure; /**< Original first callback failure; never replaced by a generic status. */
        std::optional<AiSchedulingFailureOrigin> firstFailureOrigin; /**< Set together with firstFailure. */
        std::optional<AiSchedulingAdvisoryFailure> firstAdvisoryFailure;
    };

    /**
     * @brief Single-owner scene admission policy; Foundation remains the only worker scheduler.
     * @details All methods and destruction run serially on the simulation owner. Composition/registration allocate
     * bounded storage; tick sorting and callbacks allocate no scheduler storage. Best-effort priority, overdue age and
     * persistent AgentId define order; starvation promotion is explicit and deferred wakes remain coalesced. Deterministic
     * mode reserves every registration's complete per-tick allowances, evaluates every active registration in stable
     * AgentId order each consecutive tick and rejects worker submission. Best-effort workers publish through the
     * existing continuation mailbox and terminal authority. JobSystem must outlive the service and drain image pins.
     */
    class AiTaskScheduler final {
        class ConstructionKey final {
            friend class AiTaskScheduler;
            ConstructionKey() = default;
        };

    public:
        /** @brief Detached population change for one scheduler safe point.
         * @details The scheduler must outlive validation and publication. Dropping an unpublished candidate
         * releases detached inputs without changing live registrations or invoking cancellation.
         */
        class PopulationChange final {
        public:
            PopulationChange(const PopulationChange &) = delete;
            PopulationChange &operator=(const PopulationChange &) = delete;
            /** @brief Releases detached inputs or completes any published retirements. */
            ~PopulationChange();
            /** @brief Revalidates the exact population, tick and addition lifetimes. @return Success or typed stale error. */
            [[nodiscard]] Result<void> ValidatePublication() const;
            /** @brief Commits reserved additions and fences retiring worker work; invokes no evaluator callbacks.
             * @pre ValidatePublication succeeds immediately before the owning aggregate publication. */
            void Publish() noexcept;
            /** @brief Cancels retired evaluators after the owning aggregate is visible; safe to repeat. */
            void FinishRetirements() noexcept;

        private:
            friend class AiTaskScheduler;
            struct Storage;
            explicit PopulationChange(AiTaskScheduler &scheduler);
            AiTaskScheduler *scheduler_;
            std::unique_ptr<Storage> storage_;
            std::uint64_t revision_;
            std::uint64_t tick_;
        };

        /** @brief Allocates validated finite storage. @param key Factory authority. @param jobs Borrowed process jobs.
         * @param incarnation Exact scene incarnation. @param settings Validated project limits. */
        AiTaskScheduler(ConstructionKey, JobSystem &jobs, AiRuntimeIncarnation incarnation, const AiTaskSchedulerSettings &settings);
        /** @brief Validates the same scheduler settings used for runtime creation without creating jobs or Scene state.
         * @param settings Candidate finite project limits.
         * @return Success or the typed scheduler policy error.
         */
        [[nodiscard]] static Result<void> ValidateSettings(const AiTaskSchedulerSettings &settings);
        /** @brief Composes without creating threads. @param jobs Borrowed process jobs. @param incarnation Scene identity.
         * @param settings Finite validated limits. @return Owned scheduler or typed admission/storage failure. */
        [[nodiscard]] static Result<std::unique_ptr<AiTaskScheduler>> Create(JobSystem &jobs, AiRuntimeIncarnation incarnation,
                                                                             const AiTaskSchedulerSettings &settings = {});
        /** @brief Registers one exact live generation; one coalesced wake slot per agent.
         * @param agent Runtime handle. @param identity Stable ordering identity. @param policy Positive lowerable limits.
         * @param image Pin covering callbacks and destruction. @param executor Owned bounded evaluator/committer.
         * @param cancellation Agent/scene lifetime token. @return Success or typed validation/capacity/duplicate failure.
         * @details Deterministic admission requires interval one and aggregate evaluation/work/command allowances
         * within scene limits; oversubscription rejects the new registration without changing existing registrations.
         * @pre Before the first decision phase or after current intent dispatch; never between phases. */
        [[nodiscard]] Result<void> Register(AgentHandle agent, AgentId identity, const AiAgentSchedulePolicy &policy,
                                            std::shared_ptr<const void> image, std::shared_ptr<IAiScheduledDecision> executor,
                                            CancellationToken cancellation = {});
        /** @brief Stages all population changes and checks the final deterministic allowances without touching live entries.
         * @param additions Exact new generation bindings; resources are retained by the candidate.
         * @param removals Exact Scene agent generations; valid agents without a scheduler registration require no work.
         * @return Detached change or typed validation, capacity or storage failure.
         * @pre Additions require the first decision phase to be pending or the current intent dispatch to be complete.
         * Retirements alone can also fence already evaluated intents before dispatch. No changes run during callbacks.
         */
        [[nodiscard]] Result<std::unique_ptr<PopulationChange>> PreparePopulationChange(
            std::span<const AiScheduledDecisionRegistration> additions, std::span<const AgentHandle> removals);
        /** @brief Coalesces causes without queue growth. @param agent Exact registered handle. @param reasons Nonempty causes.
         * @return Success or typed stale/lifecycle failure. */
        [[nodiscard]] Result<void> Wake(AgentHandle agent, DecisionWakeReasons reasons);
        /** @brief Stores exact-generation gameplay facts for the next decision tick.
         * @param agent Registered live generation. @param relevance Latest owner-composed immutable facts.
         * @return Success or a typed unavailable-profile, phase or stale-agent failure.
         * @pre Before the first decision phase or after intent dispatch; no publication during callbacks.
         */
        [[nodiscard]] Result<void> SetRelevanceAtSafePoint(AgentHandle agent, const AiDecisionRelevance &relevance);
        /** @brief Atomically accepts a bounded set of owner-observed facts for the next decision tick.
         * @param updates Borrowed input in strictly increasing AgentHandle order. Omitted agents retain their latest facts.
         * @return Success or a typed phase, policy, capacity or stale-agent failure.
         * @pre Call on the scheduler owner lane before its first decision phase or after intent dispatch.
         * @post Failure preserves all previously accepted facts. Success starts no work and invokes no callbacks.
         */
        [[nodiscard]] Result<void> SetRelevanceBatchAtSafePoint(std::span<const AiDecisionRelevanceUpdate> updates);
        /** @brief Reads the current admitted tier. @param agent Exact registered generation.
         * @return Current tier or a typed stale-agent failure. */
        [[nodiscard]] Result<AiSimulationTier> SimulationTier(AgentHandle agent);
        /** @brief Cancels and removes one registration. @param agent Exact handle. @return Success or typed stale/reentrant failure. */
        [[nodiscard]] Result<void> Unregister(AgentHandle agent);
        /** @brief Executes bounded owner slices at AiDecisionEvaluate. @param tick Positive nondecreasing simulation tick.
         * @details Deterministic ticks must be consecutive after intent dispatch. Undrained prior intents reject the
         * next tick with SchedulerBudgetExhausted before advancing state or renewing any allowance.
         * @return Current cumulative report or typed clock/lifecycle/budget failure. */
        [[nodiscard]] Result<AiSchedulingReport> EvaluateAtDecision(std::uint64_t tick);
        /** @brief Executes a decision phase while withholding one exact agent's next evaluation.
         * @param tick Positive nondecreasing simulation tick.
         * @param pausedAgent Generation to hold for this call. An invalid or retired handle has no effect.
         * @details The held agent retains its pending wakes and consumes no evaluation allowance. Previously evaluated
         * intents still drain normally. Only a host with explicit session policy should use this debug entry point.
         * @return Current cumulative report or a typed clock, lifecycle or budget failure.
         */
        [[nodiscard]] Result<AiSchedulingReport> EvaluateAtDecisionWithPausedAgent(std::uint64_t tick, AgentHandle pausedAgent);
        /** @brief Commits bounded intents at AiIntentDispatch; no worker waits. @param tick Exact evaluated tick.
         * @return Cumulative report or typed phase/lifecycle failure. */
        [[nodiscard]] Result<AiSchedulingReport> CommitAtIntentDispatch(std::uint64_t tick);

        /** @brief Borrows the latest completed tick on the scheduler owner lane.
         * @return Report through the next scheduler mutation, or null before dispatch completes or after shutdown.
         */
        [[nodiscard]] const AiSchedulingReport *CompletedReportAtSafePoint() const noexcept;
        /** @brief Admits detached computation from the currently executing owner slice only.
         * @param continuation Exact current task/agent identity. @param work Immutable budgeted work/image lease.
         * @details Declared worker units are charged from the current owner slice before Foundation admission.
         * @return Success or typed deterministic-mode/budget/capacity/job failure; rejection creates no job. */
        [[nodiscard]] Result<void> SubmitWorker(AiTaskContinuation continuation, AiWorkerSliceLease work);
        /** @brief Closes admission, discards intents and cancels workers without waiting. */
        void Shutdown() noexcept;
        /** @brief Closes admission while worker leases continue pinning inputs/code. */
        ~AiTaskScheduler();
        AiTaskScheduler(const AiTaskScheduler &) = delete;
        AiTaskScheduler &operator=(const AiTaskScheduler &) = delete;

    private:
        friend class AiSceneRuntime;
        friend class AiRuntimeCapability;
        friend struct Detail::AiSceneRuntimeState;
        /** @brief Queues a committed dependency of the exact reactive plan for the next decision tick. */
        void QueueBlackboardWake(AgentHandle agent, const std::shared_ptr<const CookedDecisionPlan> &plan,
                                 std::span<const BlackboardKeyId> changedKeys) noexcept;
        /** @brief Replaces the exact reactive registration's validated listener dependencies. */
        [[nodiscard]] Result<void> SetPerceptionWatches(AgentHandle agent, const std::shared_ptr<const CookedDecisionPlan> &plan,
                                                        std::vector<PerceptionListenerTypeId> listeners);
        /** @brief Borrows current listeners only on the Scene owner lane, outside native sensing calls. */
        [[nodiscard]] std::span<const PerceptionListenerTypeId> PerceptionWatches(
            AgentHandle agent, const std::shared_ptr<const CookedDecisionPlan> &plan) noexcept;
        /** @brief Borrows the selected registered evaluator after the tick has completed. */
        [[nodiscard]] const IAiScheduledDecision *InspectDecision(AgentHandle agent, const std::shared_ptr<const void> &image) noexcept;
        /** @brief Borrows the evaluator only for an exact registered image at an eligible decision replacement boundary. */
        [[nodiscard]] Result<IAiScheduledDecision *> ValidateDecisionReplacement(AgentHandle agent,
                                                                                 const std::shared_ptr<const void> &previousImage);
        /** @brief Rebinds the validated existing evaluator to its compatible plan without retiring the executor.
         * @pre ValidateDecisionReplacement immediately preceded a no-callback compatible plan update. */
        void PublishRetainedDecisionImage(AgentHandle agent, std::shared_ptr<const void> image) noexcept;
        /** @brief Reserves one exact same-agent decision replacement for the Scene owner's aggregate commit. */
        [[nodiscard]] Result<std::unique_ptr<PopulationChange>> PrepareDecisionReplacement(
            const AiScheduledDecisionRegistration &replacement, const std::shared_ptr<const void> &previousImage);
        struct Entry;
        struct RelevanceSlot;
        /** @brief Locates an exact handle in the active registration index, including the insertion point. */
        [[nodiscard]] std::vector<std::size_t>::iterator HandlePosition(AgentHandle agent) noexcept;
        /** @brief Resolves exact registration. @param agent Exact handle. @return Slot or null. */
        [[nodiscard]] Entry *Find(AgentHandle agent) noexcept;
        /** @brief Resolves a registration whose exact executable image and lifetime are both current. */
        [[nodiscard]] Entry *FindBoundDecision(AgentHandle agent, const std::shared_ptr<const void> &image) noexcept;
        /** @brief Advances one admitted owner slice, retaining bounded intents and original failures. @param entry Admitted agent. */
        void EvaluateEntry(Entry &entry);
        /** @brief Commits one retained bounded intent prefix. @param entry Exact live agent. */
        void CommitEntry(Entry &entry);
        /** @brief Validates phase and full-rate tick progression without mutation. @param tick Requested simulation tick.
         * @return Success or typed phase/backlog failure. */
        [[nodiscard]] Result<void> ValidateDecisionTick(std::uint64_t tick) const;
        /** @brief Allows new population admission only at the scheduler owner boundary. */
        [[nodiscard]] bool CanChangePopulation(bool hasAdditions) const noexcept;
        /** @brief Validates one exact live registration and its lowerable agent limits. */
        [[nodiscard]] bool ValidRegistration(const AiScheduledDecisionRegistration &registration) const noexcept;
        /** @brief Installs one admission into previously reserved scheduler storage. */
        void Install(std::size_t slot, Entry entry) noexcept;
        /** @brief Revokes a live registration, returning its callback pin for retirement. */
        [[nodiscard]] Entry Detach(Entry &entry) noexcept;
        /** @brief Checks aggregate full-rate admission. @param policy New registration limits. @return Whether all agents fit. */
        [[nodiscard]] bool FitsDeterministic(const AiAgentSchedulePolicy &policy) const noexcept;
        /** @brief Compares stable priority, age and persistent identity. @param a Left slot. @param b Right slot.
         * @return Whether the left slot precedes the right. */
        [[nodiscard]] bool Before(const Entry &a, const Entry &b) const noexcept;
        /** @brief Validates the complete exact-generation relevance batch before any facts are stored. */
        [[nodiscard]] Result<void> ValidateRelevanceUpdates(std::span<const AiDecisionRelevanceUpdate> updates);
        /** @brief Applies promotions and stable demotions before any worker publication for the tick. */
        void ApplyRelevance();
        /** @brief Reads the compact tier state, or Full when no profile was selected. */
        [[nodiscard]] AiSimulationTier CurrentTier(const Entry &entry) const noexcept;
        /** @brief Applies a changed tier, retaining the registration and owner lifetime. */
        void ChangeTier(Entry &entry, AiSimulationTier tier) noexcept;
        /** @brief Resolves periodic cadence; zero means external events only or suspension. */
        [[nodiscard]] std::uint64_t PeriodicInterval(const Entry &entry) const noexcept;
        /** @brief Builds stable priority/age/identity order into reserved storage. */
        void Order(bool admitReactiveWakes);
        /** @brief Admits eligible timer and mailbox wakes at the frozen decision boundary. */
        void AdmitReactiveWakes(Entry &entry) noexcept;
        /** @brief Runs an eligible entry without charging a held generation or losing its wake. */
        void EvaluateReadyEntry(Entry &entry, std::uint64_t tick, AgentHandle pausedAgent);
        /** @brief Rechecks lifetime immediately around callbacks. @param entry Owned slot. @return Whether retired. */
        [[nodiscard]] bool RetireIfCancelled(Entry &entry) noexcept;
        /** @brief Fences one cancellation and releases callback inputs. @param entry Exact owned slot. */
        void Retire(Entry &entry) noexcept;
        AiRuntimeIncarnation incarnation_;
        AiTaskSchedulerSettings settings_;
        std::unique_ptr<AiTaskJobService> jobs_;
        std::vector<Entry> entries_;
        std::vector<RelevanceSlot> relevance_; /**< Dense simulation-profile state, indexed by canonical slot. */
        std::vector<std::size_t> activeByHandle_; /**< Exact handle lookup for high-frequency relevance publications. */
        std::vector<std::size_t> order_;
        AiSchedulingReport report_;
        std::uint64_t populationRevision_{};
        Entry *evaluating_{};
        AiWorkBudget *evaluatingBudget_{};
        bool inPhase_{}, committed_{}, closed_{}, retired_{};
    };
}  // namespace Horo::AI
