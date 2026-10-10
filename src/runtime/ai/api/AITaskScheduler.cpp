#include "Horo/AI/AITaskScheduler.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        /** @brief Validates finite scene admission independently of runtime/backend availability. */
        [[nodiscard]] bool Valid(const AiTaskSchedulerSettings &s) noexcept {
            return s.maximumAgents > 0 && s.maximumAgents <= AiTaskSchedulerSettings::HardAgents && s.evaluationsPerTick > 0 &&
                   s.evaluationsPerTick <= s.maximumAgents && s.workUnitsPerTick > 0 &&
                   s.workUnitsPerTick <= AiTaskSchedulerSettings::HardWorkUnits && s.commandsPerTick > 0 &&
                   s.commandsPerTick <= AiTaskSchedulerSettings::HardWorkUnits && s.workerSubmissionsPerTick > 0 &&
                   s.workerSubmissionsPerTick <= AiTaskJobService::HardCapacity && s.pendingWorkers > 0 &&
                   s.pendingWorkers <= AiTaskJobService::HardCapacity && s.starvationTicks > 0 && s.mode < AiSchedulingMode::Count;
        }

        /** @brief Advances cadence without overflow or missed-interval catch-up. */
        [[nodiscard]] std::uint64_t Next(const std::uint64_t tick, const std::uint64_t interval) noexcept {
            return interval > std::numeric_limits<std::uint64_t>::max() - tick ? 0 : tick + interval;
        }
    }  // namespace

    struct AiTaskScheduler::Entry final {
        AgentHandle agent;
        AgentId identity;
        AiAgentSchedulePolicy policy;
        CancellationToken cancellation;
        std::shared_ptr<const void> image;  // Code remains pinned through executor destruction.
        std::shared_ptr<IAiScheduledDecision> executor;
        DecisionWakeReasons pending{.activation = true};
        AiDecisionSliceContext context;
        std::uint64_t nextTick{1}, waitingSince{1}, evaluatedTick{}, intentSince{};
        std::size_t submitted{}, commands{};
        bool intents{};
    };

    /** @copydoc AiWorkBudget::Consume */
    bool AiWorkBudget::Consume(const std::size_t units) noexcept {
        if (units == 0 || units > remaining_) {
            exhausted_ = true;
            return false;
        }
        remaining_ -= units;
        return true;
    }

    /** @copydoc IAiBudgetedTaskJob::Run */
    Result<void> IAiBudgetedTaskJob::Run(const AiTaskOperationContext &operation, const CancellationToken &cancellation) const {
        if (units_ == 0 || units_ > AiTaskSchedulerSettings::HardWorkUnits)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        if (cancellation.IsCancellationRequested())
            return JobCancelled();
        AiWorkBudget budget{units_};
        auto result = RunSlice(operation, cancellation, budget);
        if (result.HasError())
            return result;
        if (budget.Exhausted())
            return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        return result;
    }

    /** @copydoc AiTaskScheduler::AiTaskScheduler */
    AiTaskScheduler::AiTaskScheduler(ConstructionKey, JobSystem &jobs, const AiRuntimeIncarnation incarnation,
                                     const AiTaskSchedulerSettings settings)
        : incarnation_(incarnation), settings_(settings), entries_(settings.maximumAgents) {
        order_.reserve(settings.maximumAgents);
        auto service = AiTaskJobService::Create(jobs, settings.pendingWorkers);
        if (service.HasError())
            throw std::bad_alloc{};
        jobs_ = std::move(service).Value();
    }

    /** @copydoc AiTaskScheduler::Create */
    Result<std::unique_ptr<AiTaskScheduler>> AiTaskScheduler::Create(JobSystem &jobs, const AiRuntimeIncarnation incarnation,
                                                                     const AiTaskSchedulerSettings settings) {
        if (!incarnation.IsValid() || !Valid(settings))
            return Result<std::unique_ptr<AiTaskScheduler>>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        try {
            return Result<std::unique_ptr<AiTaskScheduler>>::Success(
                std::make_unique<AiTaskScheduler>(ConstructionKey{}, jobs, incarnation, settings));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AiTaskScheduler>>::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiTaskScheduler::Find */
    AiTaskScheduler::Entry *AiTaskScheduler::Find(const AgentHandle agent) noexcept {
        const auto found = std::find_if(entries_.begin(), entries_.end(), [agent](const Entry &entry) {
            return entry.agent == agent;
        });
        return found == entries_.end() ? nullptr : &*found;
    }

    /** @copydoc AiTaskScheduler::Register */
    Result<void> AiTaskScheduler::Register(const AgentHandle agent, const AgentId identity, const AiAgentSchedulePolicy policy,
                                           std::shared_ptr<const void> image, std::shared_ptr<IAiScheduledDecision> executor,
                                           CancellationToken cancellation) {
        if (closed_ || inPhase_ || (report_.tick != 0 && !committed_) || !agent.IsValid() || agent.incarnation != incarnation_ ||
            !identity.IsValid() || !image || !executor || cancellation.IsCancellationRequested() ||
            policy.priority >= AiTaskPriority::Count || policy.intervalTicks == 0 || policy.workUnitsPerTick == 0 ||
            policy.workUnitsPerTick > settings_.workUnitsPerTick || policy.commandsPerTick == 0 ||
            policy.commandsPerTick > settings_.commandsPerTick || policy.workerSubmissionsPerTick == 0 ||
            policy.workerSubmissionsPerTick > settings_.workerSubmissionsPerTick || policy.pendingWorkers == 0 ||
            policy.pendingWorkers > settings_.pendingWorkers)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        Entry *free = nullptr;
        for (auto &entry : entries_) {
            if (entry.executor && (entry.agent == agent || entry.identity == identity))
                return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));
            if (!entry.executor)
                free = &entry;
        }
        if (!free)
            return Result<void>::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        free->agent = agent;
        free->identity = identity;
        free->policy = policy;
        free->cancellation = std::move(cancellation);
        free->image = std::move(image);
        free->executor = std::move(executor);
        free->waitingSince = report_.tick == 0 ? 1 : report_.tick;
        free->nextTick = free->waitingSince;
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::Wake */
    Result<void> AiTaskScheduler::Wake(const AgentHandle agent, const DecisionWakeReasons reasons) {
        auto *entry = Find(agent);
        if (closed_ || inPhase_ || !entry || !entry->executor || entry->cancellation.IsCancellationRequested() || !reasons.Any())
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (!entry->pending.Any())
            entry->waitingSince = report_.tick == 0 ? 1 : report_.tick;
        entry->pending.activation |= reasons.activation;
        entry->pending.blackboard |= reasons.blackboard;
        entry->pending.perception |= reasons.perception;
        entry->pending.task |= reasons.task;
        entry->pending.polling |= reasons.polling;
        entry->pending.explicitRequest |= reasons.explicitRequest;
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::Retire */
    void AiTaskScheduler::Retire(Entry &entry) noexcept {
        jobs_->CancelAgent(entry.agent, AiTaskCancellationReason::OwnerShutdown);
        entry.executor->Cancel();
        entry.executor.reset();
        entry.image.reset();
        entry = {};
    }

    /** @copydoc AiTaskScheduler::Unregister */
    Result<void> AiTaskScheduler::Unregister(const AgentHandle agent) {
        auto *entry = Find(agent);
        if (closed_ || inPhase_ || !entry || !entry->executor)
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        inPhase_ = true;
        Retire(*entry);
        inPhase_ = false;
        if (closed_)
            Shutdown();
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::Order */
    void AiTaskScheduler::Order() {
        order_.clear();
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            auto &entry = entries_[i];
            if (!entry.executor)
                continue;
            if (entry.cancellation.IsCancellationRequested()) {
                Retire(entry);
                ++report_.cancelled;
                continue;
            }
            if (entry.nextTick != 0 && entry.nextTick <= report_.tick) {
                if (!entry.pending.Any())
                    entry.waitingSince = entry.nextTick;
                entry.pending.polling = true;
            }
            order_.push_back(i);
        }
        std::sort(order_.begin(), order_.end(), [this](const auto lhs, const auto rhs) {
            const auto &a = entries_[lhs];
            const auto &b = entries_[rhs];
            const auto aSince = a.intents ? std::min(a.waitingSince, a.intentSince) : a.waitingSince;
            const auto bSince = b.intents ? std::min(b.waitingSince, b.intentSince) : b.waitingSince;
            const bool aStarved = (a.pending.Any() || a.intents) && report_.tick - aSince >= settings_.starvationTicks;
            const bool bStarved = (b.pending.Any() || b.intents) && report_.tick - bSince >= settings_.starvationTicks;
            if (aStarved != bStarved)
                return aStarved;
            if (aStarved && aSince != bSince)
                return aSince < bSince;
            if (a.policy.priority != b.policy.priority)
                return a.policy.priority > b.policy.priority;
            if (a.waitingSince != b.waitingSince)
                return a.waitingSince < b.waitingSince;
            return a.identity < b.identity;
        });
    }

    /** @copydoc AiTaskScheduler::EvaluateAtDecision */
    Result<AiSchedulingReport> AiTaskScheduler::EvaluateAtDecision(const std::uint64_t tick) {
        if (closed_ || inPhase_ || tick == 0 || tick < report_.tick || (tick == report_.tick && committed_))
            return Result<AiSchedulingReport>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (tick != report_.tick) {
            report_ = {.tick = tick};
            committed_ = false;
            for (auto &entry : entries_) {
                entry.submitted = 0;
                entry.commands = 0;
            }
        }
        inPhase_ = true;
        jobs_->Pump();
        Order();
        report_.deferred = report_.starved = 0;
        for (const auto index : order_) {
            if (closed_)
                break;
            auto &entry = entries_[index];
            if (!entry.pending.Any())
                continue;
            if (tick - entry.waitingSince >= settings_.starvationTicks)
                ++report_.starved;
            if (entry.intents || entry.evaluatedTick == tick || report_.evaluated == settings_.evaluationsPerTick ||
                report_.workUnits == settings_.workUnitsPerTick) {
                ++report_.deferred;
                continue;
            }
            const auto units = std::min(entry.policy.workUnitsPerTick, settings_.workUnitsPerTick - report_.workUnits);
            // Reserve the whole slice before callbacks; unused allowance cannot hide unbounded reentry/work.
            report_.workUnits += units;
            ++report_.evaluated;
            entry.evaluatedTick = tick;
            entry.context = {entry.agent, tick, entry.pending, entry.cancellation};
            entry.pending = {};
            entry.nextTick = Next(tick, entry.policy.intervalTicks);
            AiWorkBudget budget{units};
            evaluating_ = &entry;
            evaluatingBudget_ = &budget;
            const auto evaluated = entry.executor->Evaluate(entry.context, budget);
            evaluating_ = nullptr;
            evaluatingBudget_ = nullptr;
            if (closed_)
                break;
            if (evaluated.HasError() || evaluated.Value() >= AiSliceOutcome::Count) {
                ++report_.failed;
                if (!report_.firstFailure)
                    report_.firstFailure = evaluated.HasError() ? evaluated.ErrorValue() : MakeError(AIErrors::SchedulerPolicyInvalid);
                entry.executor->Cancel();
                continue;
            }
            entry.intents = true;
            entry.intentSince = tick;
            const auto outcome = evaluated.Value();
            if (budget.Exhausted() || outcome == AiSliceOutcome::BudgetExhausted)
                ++report_.exhausted;
            if (outcome != AiSliceOutcome::Complete || budget.Exhausted()) {
                ++report_.yielded;
                entry.pending.explicitRequest = true;
                entry.waitingSince = tick;
            }
        }
        inPhase_ = false;
        if (closed_)
            Shutdown();
        return Result<AiSchedulingReport>::Success(report_);
    }

    /** @copydoc AiTaskScheduler::CommitAtIntentDispatch */
    Result<AiSchedulingReport> AiTaskScheduler::CommitAtIntentDispatch(const std::uint64_t tick) {
        if (closed_ || inPhase_ || tick == 0 || tick != report_.tick)
            return Result<AiSchedulingReport>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (committed_)
            return Result<AiSchedulingReport>::Success(report_);
        inPhase_ = true;
        Order();
        for (const auto index : order_) {
            if (closed_)
                break;
            auto &entry = entries_[index];
            if (!entry.intents || entry.commands == entry.policy.commandsPerTick || report_.commands == settings_.commandsPerTick)
                continue;
            const auto units = std::min(entry.policy.commandsPerTick - entry.commands, settings_.commandsPerTick - report_.commands);
            AiWorkBudget budget{units};
            const auto committed = entry.executor->Commit(entry.context, budget);
            if (closed_)
                break;
            // Reserve the finite command slice, even if the callback yields without consuming.
            const auto consumed = units;
            entry.commands += consumed;
            report_.commands += consumed;
            if (committed.HasError()) {
                ++report_.failed;
                if (!report_.firstFailure)
                    report_.firstFailure = committed.ErrorValue();
                entry.executor->Cancel();
                entry.intents = false;
            } else {
                entry.intents = !committed.Value();
                if (entry.intents)
                    ++report_.exhausted;
            }
        }
        committed_ = true;
        inPhase_ = false;
        if (closed_)
            Shutdown();
        return Result<AiSchedulingReport>::Success(report_);
    }

    /** @copydoc AiTaskScheduler::SubmitWorker */
    Result<void> AiTaskScheduler::SubmitWorker(AiTaskContinuation continuation, AiWorkerSliceLease work) {
        if (closed_ || !evaluating_ || evaluating_->cancellation.IsCancellationRequested() ||
            continuation.Operation().agent != evaluating_->agent)
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (settings_.mode == AiSchedulingMode::Deterministic)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerWorkerUnsupported));
        if (evaluating_->submitted == evaluating_->policy.workerSubmissionsPerTick ||
            report_.workerSubmissions == settings_.workerSubmissionsPerTick ||
            jobs_->PendingCount(evaluating_->agent) >= evaluating_->policy.pendingWorkers)
            return Result<void>::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        if (!work.lease_.IsValid() || work.units_ == 0 || work.units_ > evaluatingBudget_->Remaining())
            return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        static_cast<void>(evaluatingBudget_->Consume(work.units_));
        auto started = jobs_->Start(std::move(continuation), std::move(work.lease_));
        if (started.HasValue()) {
            ++evaluating_->submitted;
            ++report_.workerSubmissions;
        }
        return started;
    }

    /** @copydoc AiTaskScheduler::Shutdown */
    void AiTaskScheduler::Shutdown() noexcept {
        closed_ = true;
        if (inPhase_ || retired_)
            return;
        retired_ = true;
        jobs_->Shutdown();
        for (auto &entry : entries_)
            if (entry.executor)
                Retire(entry);
    }

    /** @copydoc AiTaskScheduler::~AiTaskScheduler */
    AiTaskScheduler::~AiTaskScheduler() {
        Shutdown();
    }
}  // namespace Horo::AI
