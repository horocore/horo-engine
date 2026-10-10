#pragma once

/** @file AITaskScheduler.h
 * @brief Scene-owned fixed-tick admission and cooperative slicing over the Foundation JobSystem.
 */
#include "Horo/AI/AITaskJobService.h"
#include "Horo/AI/DecisionWakePolicy.h"

namespace Horo::AI {
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
    /** @brief Execution selection; deterministic admission excludes timing-dependent worker execution. */
    enum class AiSchedulingMode : std::uint8_t {
        Deterministic,
        BestEffort,
        Count
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
         * @param operation Exact detached task. @param cancellation Cooperative worker ancestry. @param budget Finite work allowance.
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
        /** @brief Publishes a bounded prefix of evaluated intents. @param context Evaluation identity, revalidated by the scheduler.
         * @param budget Command allowance. @return True when all intents are committed, false to retain the bounded remainder. */
        [[nodiscard]] virtual Result<bool> Commit(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept = 0;
        /** @brief Discards retained intents and transient evaluation at cancellation. Must be idempotent. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Lowerable per-agent frequency, work, command and worker-admission policy. */
    struct AiAgentSchedulePolicy final {
        AiTaskPriority priority{AiTaskPriority::Normal};
        std::uint64_t intervalTicks{1}; /**< Missed intervals coalesce; no catch-up work. */
        std::size_t workUnitsPerTick{64};
        std::size_t commandsPerTick{16};
        std::size_t workerSubmissionsPerTick{1};
        std::size_t pendingWorkers{1};
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
    };

    /** @brief Observable bounded outcomes for the most recent fixed tick; repeated calls never replenish it. */
    struct AiSchedulingReport final {
        std::uint64_t tick{};
        std::size_t evaluated{}, workUnits{}, commands{}, workerSubmissions{};
        std::size_t deferred{}, starved{}, exhausted{}, yielded{}, failed{}, cancelled{};
        std::optional<Error> firstFailure; /**< Original first callback failure; never replaced by a generic status. */
    };

    /**
     * @brief Single-owner scene admission policy; Foundation remains the only worker scheduler.
     * @details All methods and destruction run serially on the simulation owner. Composition/registration allocate
     * bounded storage; tick sorting and callbacks allocate no scheduler storage. Priority, overdue age and persistent
     * AgentId define order; starvation promotion is explicit. Budget-deferred wakes remain coalesced. Deterministic
     * mode uses cooperative owner evaluation and rejects worker submission; best-effort workers publish through the
     * existing continuation mailbox and terminal authority. JobSystem must outlive the service and drain image pins.
     */
    class AiTaskScheduler final {
        class ConstructionKey final {
            friend class AiTaskScheduler;
            ConstructionKey() = default;
        };

    public:
        /** @brief Allocates validated finite storage. @param key Factory authority. @param jobs Borrowed process jobs.
         * @param incarnation Exact scene incarnation. @param settings Validated project limits. */
        AiTaskScheduler(ConstructionKey, JobSystem &jobs, AiRuntimeIncarnation incarnation, const AiTaskSchedulerSettings &settings);
        /** @brief Composes without creating threads. @param jobs Borrowed process jobs. @param incarnation Scene identity.
         * @param settings Finite validated limits. @return Owned scheduler or typed admission/storage failure. */
        [[nodiscard]] static Result<std::unique_ptr<AiTaskScheduler>> Create(JobSystem &jobs, AiRuntimeIncarnation incarnation,
                                                                             const AiTaskSchedulerSettings &settings = {});
        /** @brief Registers one exact live generation; one coalesced wake slot per agent.
         * @param agent Runtime handle. @param identity Stable ordering identity. @param policy Positive lowerable limits.
         * @param image Pin covering callbacks and destruction. @param executor Owned bounded evaluator/committer.
         * @param cancellation Agent/scene lifetime token. @return Success or typed validation/capacity/duplicate failure.
         * @pre Before the first decision phase or after current intent dispatch; never between phases. */
        [[nodiscard]] Result<void> Register(AgentHandle agent, AgentId identity, const AiAgentSchedulePolicy &policy,
                                            std::shared_ptr<const void> image, std::shared_ptr<IAiScheduledDecision> executor,
                                            CancellationToken cancellation = {});
        /** @brief Coalesces causes without queue growth. @param agent Exact registered handle. @param reasons Nonempty causes.
         * @return Success or typed stale/lifecycle failure. */
        [[nodiscard]] Result<void> Wake(AgentHandle agent, DecisionWakeReasons reasons);
        /** @brief Cancels and removes one registration. @param agent Exact handle. @return Success or typed stale/reentrant failure. */
        [[nodiscard]] Result<void> Unregister(AgentHandle agent);
        /** @brief Executes bounded owner slices at AiDecisionEvaluate. @param tick Positive nondecreasing simulation tick.
         * @return Current cumulative report or typed clock/lifecycle failure. */
        [[nodiscard]] Result<AiSchedulingReport> EvaluateAtDecision(std::uint64_t tick);
        /** @brief Commits bounded intents at AiIntentDispatch; no worker waits. @param tick Exact evaluated tick.
         * @return Cumulative report or typed phase/lifecycle failure. */
        [[nodiscard]] Result<AiSchedulingReport> CommitAtIntentDispatch(std::uint64_t tick);
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
        struct Entry;
        /** @brief Resolves exact registration. @param agent Exact handle. @return Slot or null. */
        [[nodiscard]] Entry *Find(AgentHandle agent) noexcept;
        /** @brief Advances one admitted owner slice, retaining bounded intents and original failures. @param entry Admitted agent. */
        void EvaluateEntry(Entry &entry);
        /** @brief Commits one retained bounded intent prefix. @param entry Exact live agent. */
        void CommitEntry(Entry &entry);
        /** @brief Compares stable priority, age and persistent identity. @param a Left slot. @param b Right slot.
         * @return Whether the left slot precedes the right. */
        [[nodiscard]] bool Before(const Entry &a, const Entry &b) const noexcept;
        /** @brief Builds stable priority/age/identity order into reserved storage. */
        void Order();
        /** @brief Rechecks lifetime immediately around callbacks. @param entry Owned slot. @return Whether retired. */
        [[nodiscard]] bool RetireIfCancelled(Entry &entry) noexcept;
        /** @brief Fences one cancellation and releases callback inputs. @param entry Exact owned slot. */
        void Retire(Entry &entry) noexcept;
        AiRuntimeIncarnation incarnation_;
        AiTaskSchedulerSettings settings_;
        std::unique_ptr<AiTaskJobService> jobs_;
        std::vector<Entry> entries_;
        std::vector<std::size_t> order_;
        AiSchedulingReport report_;
        Entry *evaluating_{};
        AiWorkBudget *evaluatingBudget_{};
        bool inPhase_{}, committed_{}, closed_{}, retired_{};
    };
}  // namespace Horo::AI
