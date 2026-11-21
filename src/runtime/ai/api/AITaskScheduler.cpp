#include "Horo/AI/AITaskScheduler.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Checks increasing bands and strictly positive best-effort cadence settings. */
        [[nodiscard]] bool ValidProfile(const AiDecisionSimulationProfile &profile) noexcept {
            return profile.fullDistanceMillimeters > 0 && profile.fullDistanceMillimeters < profile.reducedDistanceMillimeters &&
                   profile.reducedDistanceMillimeters < profile.suspensionDistanceMillimeters && profile.reducedIntervalTicks > 1 &&
                   profile.demotionDelayTicks > 0;
        }

        /** @brief Validates finite scene admission independently of runtime/backend availability. */
        [[nodiscard]] bool Valid(const AiTaskSchedulerSettings &s) noexcept {
            return s.maximumAgents > 0 && s.maximumAgents <= AiTaskSchedulerSettings::HardAgents && s.evaluationsPerTick > 0 &&
                   s.evaluationsPerTick <= s.maximumAgents && s.workUnitsPerTick > 0 &&
                   s.workUnitsPerTick <= AiTaskSchedulerSettings::HardWorkUnits && s.commandsPerTick > 0 &&
                   s.commandsPerTick <= AiTaskSchedulerSettings::HardWorkUnits && s.workerSubmissionsPerTick > 0 &&
                   s.workerSubmissionsPerTick <= AiTaskJobService::HardCapacity && s.pendingWorkers > 0 &&
                   s.pendingWorkers <= AiTaskJobService::HardCapacity && s.starvationTicks > 0 && s.mode < AiSchedulingMode::Count &&
                   (!s.simulationProfile || (s.mode == AiSchedulingMode::BestEffort && ValidProfile(*s.simulationProfile)));
        }

        /** @brief Uses only host-captured simulation facts and conservatively preserves unknown relevance. */
        [[nodiscard]] AiSimulationTier DesiredTier(const AiDecisionSimulationProfile &profile,
                                                   const AiDecisionRelevance &relevance) noexcept {
            if (relevance.gameplayPinned || relevance.urgentStimulus || !relevance.anchorDistanceMillimeters ||
                *relevance.anchorDistanceMillimeters <= profile.fullDistanceMillimeters)
                return AiSimulationTier::Full;
            const auto distance = *relevance.anchorDistanceMillimeters;
            if (distance <= profile.reducedDistanceMillimeters)
                return AiSimulationTier::Reduced;
            if (distance <= profile.suspensionDistanceMillimeters || relevance.activeSimulationRegion)
                return AiSimulationTier::EventOnly;
            return AiSimulationTier::Suspended;
        }

        /** @brief Only trusted event or explicit owner wakes can reactivate a suspended registration. */
        [[nodiscard]] bool WakesSuspended(const DecisionWakeReasons &reasons) noexcept {
            return reasons.perception || reasons.explicitRequest;
        }

        /** @brief Remaining deterministic allowance after each admitted registration. */
        struct DeterministicAllowance final {
            std::size_t evaluations;
            std::size_t work;
            std::size_t commands;

            [[nodiscard]] bool Reserve(const AiAgentSchedulePolicy &policy) noexcept {
                if (evaluations == 0 || policy.workUnitsPerTick > work || policy.commandsPerTick > commands)
                    return false;
                --evaluations;
                work -= policy.workUnitsPerTick;
                commands -= policy.commandsPerTick;
                return true;
            }
        };

        /** @brief Checks sorted admission keys for duplicates. */
        template <typename Key> bool Unique(std::vector<Key> &keys) {
            std::ranges::sort(keys);
            return std::ranges::adjacent_find(keys) == keys.end();
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
        AiDecisionRelevance relevance;
        AiSimulationTier tier{AiSimulationTier::Full};
        AiSimulationTier demotionTarget{AiSimulationTier::Full};
        std::uint64_t demotionSince{};
        AiDecisionSliceContext context;
        std::uint64_t nextTick{1};
        std::uint64_t waitingSince{1};
        std::uint64_t evaluatedTick{};
        std::uint64_t intentSince{};
        std::size_t submitted{};
        std::size_t commands{};
        bool intents{};
    };

    struct AiTaskScheduler::PopulationChange::Storage final {
        std::vector<std::size_t> removals;
        std::vector<std::pair<std::size_t, Entry>> additions;
        std::vector<Entry> retired;
        bool decisionReplacement{};
        AiTaskScheduler *retirementOwner{};

        /** @brief Resolves known exact retiring generations without mutating scheduler state. */
        [[nodiscard]] Result<void> PrepareRemovals(AiTaskScheduler &scheduler, std::span<const AgentHandle> requested) {
            removals.reserve(requested.size());
            for (const auto agent : requested) {
                if (!agent.IsValid() || agent.incarnation != scheduler.incarnation_)
                    return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
                if (const auto *entry = scheduler.Find(agent))
                    removals.push_back(static_cast<std::size_t>(entry - scheduler.entries_.data()));
            }
            if (!Unique(removals))
                return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));
            retired.reserve(removals.size());
            return Result<void>::Success();
        }

        /** @brief Resolves final capacity, uniqueness and deterministic allowances before staging any admission. */
        [[nodiscard]] Result<void> PrepareAdditions(AiTaskScheduler &scheduler, std::span<const AiScheduledDecisionRegistration> requested,
                                                    const Entry *predecessor = nullptr) {
            if (requested.empty())
                return Result<void>::Success();

            const auto &settings = scheduler.settings_;
            const auto residents = scheduler.activeByHandle_.size() - removals.size();
            if (requested.size() > settings.maximumAgents - residents)
                return Result<void>::Failure(MakeError(AIErrors::AgentCapacityExceeded));

            std::vector<AgentHandle> agents;
            std::vector<AgentId> identities;
            std::vector<std::size_t> available;
            agents.reserve(residents + requested.size());
            identities.reserve(residents + requested.size());
            available.reserve(requested.size());
            DeterministicAllowance allowance{settings.evaluationsPerTick, settings.workUnitsPerTick, settings.commandsPerTick};
            const bool deterministic = settings.mode == AiSchedulingMode::Deterministic;
            for (std::size_t index = 0; index < scheduler.entries_.size(); ++index) {
                const auto &entry = scheduler.entries_[index];
                if (!entry.executor || std::ranges::binary_search(removals, index)) {
                    if (available.size() < requested.size())
                        available.push_back(index);
                    continue;
                }
                agents.push_back(entry.agent);
                identities.push_back(entry.identity);
                if (deterministic && !allowance.Reserve(entry.policy))
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
            }
            for (const auto &input : requested) {
                if (!scheduler.ValidRegistration(input))
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
                if (const auto *active = scheduler.Find(input.agent); active && active != predecessor)
                    return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));
                if (deterministic && !allowance.Reserve(input.policy))
                    return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                agents.push_back(input.agent);
                identities.push_back(input.identity);
            }
            if (!Unique(agents) || !Unique(identities))
                return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));

            additions.reserve(requested.size());
            for (std::size_t i = 0; i < requested.size(); ++i) {
                const auto &input = requested[i];
                additions.emplace_back(available[i], Entry{.agent = input.agent,
                                                           .identity = input.identity,
                                                           .policy = input.policy,
                                                           .cancellation = input.cancellation,
                                                           .image = input.image,
                                                           .executor = input.executor});
            }
            return Result<void>::Success();
        }
    };

    AiTaskScheduler::PopulationChange::PopulationChange(AiTaskScheduler &scheduler)
        : scheduler_(&scheduler), storage_(std::make_unique<Storage>()), revision_(scheduler.populationRevision_),
          tick_(scheduler.report_.tick) {}

    /** @copydoc AiTaskScheduler::PopulationChange::~PopulationChange */
    AiTaskScheduler::PopulationChange::~PopulationChange() {
        FinishRetirements();
    }

    /** @copydoc AiTaskScheduler::PopulationChange::ValidatePublication */
    Result<void> AiTaskScheduler::PopulationChange::ValidatePublication() const {
        if (!scheduler_ || !scheduler_->CanChangePopulation(!storage_->additions.empty()) || scheduler_->populationRevision_ != revision_ ||
            scheduler_->report_.tick != tick_)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (std::ranges::any_of(storage_->additions, [](const auto &addition) {
            return addition.second.cancellation.IsCancellationRequested();
        }))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::PopulationChange::Publish */
    void AiTaskScheduler::PopulationChange::Publish() noexcept {
        if (!scheduler_)
            return;
        if (storage_->decisionReplacement) {
            storage_->retirementOwner = scheduler_;
            scheduler_->inPhase_ = true;
        }
        for (const auto index : storage_->removals)
            storage_->retired.push_back(scheduler_->Detach(scheduler_->entries_[index]));
        for (auto &[index, entry] : storage_->additions)
            scheduler_->Install(index, std::move(entry));
        scheduler_ = nullptr;
    }

    /** @copydoc AiTaskScheduler::PopulationChange::FinishRetirements */
    void AiTaskScheduler::PopulationChange::FinishRetirements() noexcept {
        auto retired = std::exchange(storage_->retired, {});
        if (auto *owner = std::exchange(storage_->retirementOwner, nullptr)) {
            for (auto &entry : retired)
                entry.executor->CancelForReplacement();
            retired.clear();
            owner->inPhase_ = false;
            if (owner->closed_)
                owner->Shutdown();
        } else {
            for (auto &entry : retired)
                entry.executor->Cancel();
        }
    }

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
        if (operation.IsCancellationRequested() || cancellation.IsCancellationRequested())
            return JobCancelled();
        AiWorkBudget budget{units_};
        auto result = RunSlice(operation, cancellation, budget);
        if (operation.IsCancellationRequested() || cancellation.IsCancellationRequested())
            return JobCancelled();
        if (result.HasError())
            return result;
        if (budget.Exhausted())
            return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        return result;
    }

    /** @copydoc AiTaskScheduler::AiTaskScheduler */
    AiTaskScheduler::AiTaskScheduler(ConstructionKey, JobSystem &jobs, const AiRuntimeIncarnation incarnation,
                                     const AiTaskSchedulerSettings &settings)
        : incarnation_(incarnation), settings_(settings), entries_(settings.maximumAgents) {
        activeByHandle_.reserve(settings.maximumAgents);
        order_.reserve(settings.maximumAgents);
        auto service = AiTaskJobService::Create(jobs, settings.pendingWorkers);
        if (service.HasError())
            throw std::bad_alloc{};
        jobs_ = std::move(service).Value();
    }

    /** @copydoc AiTaskScheduler::ValidateSettings */
    Result<void> AiTaskScheduler::ValidateSettings(const AiTaskSchedulerSettings &settings) {
        return Valid(settings) ? Result<void>::Success() : Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
    }

    /** @copydoc AiTaskScheduler::Create */
    Result<std::unique_ptr<AiTaskScheduler>> AiTaskScheduler::Create(JobSystem &jobs, const AiRuntimeIncarnation incarnation,
                                                                     const AiTaskSchedulerSettings &settings) {
        if (!incarnation.IsValid())
            return Result<std::unique_ptr<AiTaskScheduler>>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        if (const auto validated = ValidateSettings(settings); validated.HasError())
            return Result<std::unique_ptr<AiTaskScheduler>>::Failure(validated.ErrorValue());
        try {
            return Result<std::unique_ptr<AiTaskScheduler>>::Success(
                std::make_unique<AiTaskScheduler>(ConstructionKey{}, jobs, incarnation, settings));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AiTaskScheduler>>::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiTaskScheduler::HandlePosition */
    std::vector<std::size_t>::iterator AiTaskScheduler::HandlePosition(const AgentHandle agent) noexcept {
        return std::ranges::lower_bound(activeByHandle_, agent, {}, [this](const std::size_t slot) {
            return entries_[slot].agent;
        });
    }

    /** @copydoc AiTaskScheduler::Find */
    AiTaskScheduler::Entry *AiTaskScheduler::Find(const AgentHandle agent) noexcept {
        const auto found = HandlePosition(agent);
        return found != activeByHandle_.end() && entries_[*found].agent == agent ? &entries_[*found] : nullptr;
    }

    /** @copydoc AiTaskScheduler::CanChangePopulation */
    bool AiTaskScheduler::CanChangePopulation(const bool hasAdditions) const noexcept {
        return !closed_ && !inPhase_ && (!hasAdditions || report_.tick == 0 || committed_) &&
               populationRevision_ != std::numeric_limits<std::uint64_t>::max();
    }

    /** @copydoc AiTaskScheduler::ValidRegistration */
    bool AiTaskScheduler::ValidRegistration(const AiScheduledDecisionRegistration &registration) const noexcept {
        const auto &policy = registration.policy;
        return registration.agent.IsValid() && registration.agent.incarnation == incarnation_ && registration.identity.IsValid() &&
               registration.image && registration.executor && !registration.cancellation.IsCancellationRequested() &&
               policy.priority < AiTaskPriority::Count &&
               (settings_.mode != AiSchedulingMode::Deterministic || policy.intervalTicks == 1) && policy.workUnitsPerTick > 0 &&
               policy.workUnitsPerTick <= settings_.workUnitsPerTick && policy.commandsPerTick > 0 &&
               policy.commandsPerTick <= settings_.commandsPerTick && policy.workerSubmissionsPerTick > 0 &&
               policy.workerSubmissionsPerTick <= settings_.workerSubmissionsPerTick && policy.pendingWorkers > 0 &&
               policy.pendingWorkers <= settings_.pendingWorkers;
    }

    /** @copydoc AiTaskScheduler::FitsDeterministic */
    bool AiTaskScheduler::FitsDeterministic(const AiAgentSchedulePolicy &policy) const noexcept {
        DeterministicAllowance allowance{settings_.evaluationsPerTick, settings_.workUnitsPerTick, settings_.commandsPerTick};
        return std::ranges::all_of(entries_, [&allowance](const Entry &entry) {
            return !entry.executor || allowance.Reserve(entry.policy);
        }) && allowance.Reserve(policy);
    }

    /** @copydoc AiTaskScheduler::Install */
    void AiTaskScheduler::Install(const std::size_t slot, Entry entry) noexcept {
        entry.waitingSince = report_.tick == 0 ? 1 : report_.tick;
        entry.nextTick = entry.policy.intervalTicks == 0 ? 0 : entry.waitingSince;
        entries_[slot] = std::move(entry);
        activeByHandle_.insert(HandlePosition(entries_[slot].agent), slot);
        if (populationRevision_ != std::numeric_limits<std::uint64_t>::max())
            ++populationRevision_;
    }

    /** @copydoc AiTaskScheduler::Register */
    Result<void> AiTaskScheduler::Register(const AgentHandle agent, const AgentId identity, const AiAgentSchedulePolicy &policy,
                                           std::shared_ptr<const void> image, std::shared_ptr<IAiScheduledDecision> executor,
                                           CancellationToken cancellation) {
        AiScheduledDecisionRegistration captured{agent, identity, policy, std::move(image), std::move(executor), std::move(cancellation)};
        if (!CanChangePopulation(true) || !ValidRegistration(captured))
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
        if (settings_.mode == AiSchedulingMode::Deterministic && !FitsDeterministic(policy))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));

        Install(static_cast<std::size_t>(free - entries_.data()), {.agent = agent,
                                                                   .identity = identity,
                                                                   .policy = policy,
                                                                   .cancellation = std::move(captured.cancellation),
                                                                   .image = std::move(captured.image),
                                                                   .executor = std::move(captured.executor)});
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::PreparePopulationChange */
    Result<std::unique_ptr<AiTaskScheduler::PopulationChange>> AiTaskScheduler::PreparePopulationChange(
        const std::span<const AiScheduledDecisionRegistration> additions, const std::span<const AgentHandle> removals) {
        using Prepared = Result<std::unique_ptr<PopulationChange>>;
        if (!CanChangePopulation(!additions.empty()))
            return Prepared::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (additions.size() > settings_.maximumAgents || removals.size() > AiTaskSchedulerSettings::HardAgents)
            return Prepared::Failure(MakeError(AIErrors::AgentCapacityExceeded));
        try {
            std::unique_ptr<PopulationChange> candidate(new PopulationChange(*this));
            if (auto prepared = candidate->storage_->PrepareRemovals(*this, removals); prepared.HasError())
                return Prepared::Failure(prepared.ErrorValue());
            if (auto prepared = candidate->storage_->PrepareAdditions(*this, additions); prepared.HasError())
                return Prepared::Failure(prepared.ErrorValue());
            return Prepared::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Prepared::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiTaskScheduler::FindBoundDecision */
    AiTaskScheduler::Entry *AiTaskScheduler::FindBoundDecision(const AgentHandle agent, const std::shared_ptr<const void> &image) noexcept {
        auto *entry = Find(agent);
        if (!entry || !image || entry->image != image || entry->image.owner_before(image) || image.owner_before(entry->image) ||
            entry->cancellation.IsCancellationRequested())
            return nullptr;
        return entry;
    }

    /** @copydoc AiTaskScheduler::InspectDecision */
    const IAiScheduledDecision *AiTaskScheduler::InspectDecision(const AgentHandle agent,
                                                                 const std::shared_ptr<const void> &image) noexcept {
        if (!CompletedReportAtSafePoint())
            return nullptr;
        const auto *entry = FindBoundDecision(agent, image);
        return entry ? entry->executor.get() : nullptr;
    }

    /** @copydoc AiTaskScheduler::ValidateDecisionReplacement */
    Result<IAiScheduledDecision *> AiTaskScheduler::ValidateDecisionReplacement(const AgentHandle agent,
                                                                                const std::shared_ptr<const void> &previousImage) {
        using Validated = Result<IAiScheduledDecision *>;
        if (!CanChangePopulation(true) || settings_.mode != AiSchedulingMode::Deterministic ||
            report_.tick == std::numeric_limits<std::uint64_t>::max())
            return Validated::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        const auto *entry = FindBoundDecision(agent, previousImage);
        if (!entry)
            return Validated::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        return Validated::Success(entry->executor.get());
    }

    /** @copydoc AiTaskScheduler::PublishRetainedDecisionImage */
    void AiTaskScheduler::PublishRetainedDecisionImage(const AgentHandle agent, std::shared_ptr<const void> image) noexcept {
        Find(agent)->image = std::move(image);
        ++populationRevision_;
    }

    /** @copydoc AiTaskScheduler::PrepareDecisionReplacement */
    Result<std::unique_ptr<AiTaskScheduler::PopulationChange>> AiTaskScheduler::PrepareDecisionReplacement(
        const AiScheduledDecisionRegistration &replacement, const std::shared_ptr<const void> &previousImage) {
        using Prepared = Result<std::unique_ptr<PopulationChange>>;
        if (auto valid = ValidateDecisionReplacement(replacement.agent, previousImage); valid.HasError())
            return Prepared::Failure(valid.ErrorValue());
        const auto *old = Find(replacement.agent);
        if (old->identity != replacement.identity || old->image == replacement.image || old->executor == replacement.executor)
            return Prepared::Failure(MakeError(AIErrors::DecisionAssetActivationInvalid));
        try {
            std::unique_ptr<PopulationChange> candidate(new PopulationChange(*this));
            auto &storage = *candidate->storage_;
            if (auto removed = storage.PrepareRemovals(*this, std::span{&replacement.agent, 1}); removed.HasError())
                return Prepared::Failure(removed.ErrorValue());
            if (auto added = storage.PrepareAdditions(*this, std::span{&replacement, 1}, old); added.HasError())
                return Prepared::Failure(added.ErrorValue());
            storage.decisionReplacement = true;
            return Prepared::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Prepared::Failure(MakeError(AIErrors::BehaviorTreeStorageUnavailable));
        }
    }

    /** @copydoc AiTaskScheduler::Wake */
    Result<void> AiTaskScheduler::Wake(const AgentHandle agent, DecisionWakeReasons reasons) {
        auto *entry = Find(agent);
        if (closed_ || inPhase_ || !entry || !entry->executor || entry->cancellation.IsCancellationRequested() || !reasons.Any())
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        if (entry->tier == AiSimulationTier::Suspended) {
            reasons = {.perception = reasons.perception, .explicitRequest = reasons.explicitRequest};
        } else if (entry->tier == AiSimulationTier::Reduced || entry->tier == AiSimulationTier::EventOnly) {
            reasons.polling = false;
        }
        if (!reasons.Any())
            return Result<void>::Success();
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

    /** @copydoc AiTaskScheduler::SetRelevanceAtSafePoint */
    Result<void> AiTaskScheduler::SetRelevanceAtSafePoint(const AgentHandle agent, const AiDecisionRelevance &relevance) {
        if (closed_ || inPhase_ || (report_.tick != 0 && !committed_))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (!settings_.simulationProfile)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        auto *entry = Find(agent);
        if (!entry || !entry->executor || entry->cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(AIErrors::TaskContextInvalid));
        entry->relevance = relevance;
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::SimulationTier */
    Result<AiSimulationTier> AiTaskScheduler::SimulationTier(const AgentHandle agent) {
        auto *entry = Find(agent);
        if (closed_ || inPhase_ || !entry || !entry->executor || entry->cancellation.IsCancellationRequested())
            return Result<AiSimulationTier>::Failure(MakeError(AIErrors::TaskContextInvalid));
        return Result<AiSimulationTier>::Success(entry->tier);
    }

    /** @copydoc AiTaskScheduler::PeriodicInterval */
    std::uint64_t AiTaskScheduler::PeriodicInterval(const Entry &entry) const noexcept {
        const auto cadence = entry.policy.intervalTicks;
        if (cadence == 0)
            return 0;
        switch (entry.tier) {
            case AiSimulationTier::Full:
                return cadence;
            case AiSimulationTier::Reduced:
                return std::max(cadence, settings_.simulationProfile->reducedIntervalTicks);
            case AiSimulationTier::EventOnly:
            case AiSimulationTier::Suspended:
                return 0;
        }
        return 0;
    }

    /** @copydoc AiTaskScheduler::ChangeTier */
    void AiTaskScheduler::ChangeTier(Entry &entry, const AiSimulationTier tier) noexcept {
        const auto previous = entry.tier;
        entry.tier = tier;
        entry.demotionSince = 0;
        entry.pending.polling = false;
        const auto interval = PeriodicInterval(entry);
        entry.nextTick = interval == 0 ? 0 : Next(entry.evaluatedTick, interval);
        if (tier == AiSimulationTier::Suspended) {
            entry.pending = {};
            entry.intents = false;
            entry.context = {};
            jobs_->DiscardAgentWork(entry.agent);
            entry.executor->Cancel();
        } else if (tier < previous) {
            if (!entry.pending.Any())
                entry.waitingSince = report_.tick;
            entry.pending.activation = true;
        }
    }

    /** @copydoc AiTaskScheduler::ApplyRelevance */
    void AiTaskScheduler::ApplyRelevance() {
        const auto &profile = *settings_.simulationProfile;
        for (auto &entry : entries_) {
            if (closed_)
                break;
            if (!entry.executor || entry.cancellation.IsCancellationRequested())
                continue;
            auto desired = DesiredTier(profile, entry.relevance);
            if (entry.tier == AiSimulationTier::Suspended && desired == entry.tier && WakesSuspended(entry.pending))
                desired = AiSimulationTier::EventOnly;
            if (desired == entry.tier) {
                entry.demotionSince = 0;
            } else if (desired < entry.tier) {
                ChangeTier(entry, desired);
            } else if (entry.demotionSince == 0 || desired != entry.demotionTarget) {
                entry.demotionTarget = desired;
                entry.demotionSince = report_.tick;
            } else if (report_.tick - entry.demotionSince >= profile.demotionDelayTicks) {
                ChangeTier(entry, desired);
            }
        }
    }

    /** @copydoc AiTaskScheduler::Detach */
    AiTaskScheduler::Entry AiTaskScheduler::Detach(Entry &entry) noexcept {
        if (!closed_)
            activeByHandle_.erase(HandlePosition(entry.agent));
        if (populationRevision_ != std::numeric_limits<std::uint64_t>::max())
            ++populationRevision_;
        jobs_->CancelAgent(entry.agent, AiTaskCancellationReason::OwnerShutdown);
        auto retired = std::move(entry);
        entry = {};
        return retired;
    }

    /** @copydoc AiTaskScheduler::Retire */
    void AiTaskScheduler::Retire(Entry &entry) noexcept {
        auto retired = Detach(entry);
        retired.executor->Cancel();
    }

    /** @copydoc AiTaskScheduler::RetireIfCancelled */
    bool AiTaskScheduler::RetireIfCancelled(Entry &entry) noexcept {
        if (!entry.cancellation.IsCancellationRequested())
            return false;
        Retire(entry);
        ++report_.cancelled;
        return true;
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
            if (RetireIfCancelled(entry))
                continue;
            if (entry.nextTick != 0 && entry.nextTick <= report_.tick) {
                if (!entry.pending.Any())
                    entry.waitingSince = entry.nextTick;
                entry.pending.polling = true;
            }
            if (entry.pending.Any() || entry.intents)
                order_.push_back(i);
        }
        std::ranges::sort(order_, [this](const auto lhs, const auto rhs) {
            return Before(entries_[lhs], entries_[rhs]);
        });
    }

    /** @copydoc AiTaskScheduler::Before */
    bool AiTaskScheduler::Before(const Entry &a, const Entry &b) const noexcept {
        if (settings_.mode == AiSchedulingMode::Deterministic)
            return a.identity < b.identity;
        const auto aSince = a.intents ? std::min(a.waitingSince, a.intentSince) : a.waitingSince;
        const auto bSince = b.intents ? std::min(b.waitingSince, b.intentSince) : b.waitingSince;
        const bool aStarved = (a.pending.Any() || a.intents) && report_.tick - aSince >= settings_.starvationTicks;
        if (const bool bStarved = (b.pending.Any() || b.intents) && report_.tick - bSince >= settings_.starvationTicks;
            aStarved != bStarved)
            return aStarved;
        if (aStarved && aSince != bSince)
            return aSince < bSince;
        if (a.policy.priority != b.policy.priority)
            return a.policy.priority > b.policy.priority;
        if (a.waitingSince != b.waitingSince)
            return a.waitingSince < b.waitingSince;
        return a.identity < b.identity;
    }

    /** @copydoc AiTaskScheduler::EvaluateEntry */
    void AiTaskScheduler::EvaluateEntry(Entry &entry) {
        const auto tick = report_.tick;
        const auto units = std::min(entry.policy.workUnitsPerTick, settings_.workUnitsPerTick - report_.workUnits);
        // Reserve the whole slice before callbacks; unused allowance cannot hide unbounded reentry/work.
        report_.workUnits += units;
        ++report_.evaluated;
        entry.evaluatedTick = tick;
        entry.context = {entry.agent, tick, entry.pending, entry.cancellation, entry.tier};
        entry.pending = {};
        const auto interval = PeriodicInterval(entry);
        entry.nextTick = interval == 0 ? 0 : Next(tick, interval);
        AiWorkBudget budget{units};
        evaluating_ = &entry;
        evaluatingBudget_ = &budget;
        const auto evaluated = entry.executor->Evaluate(entry.context, budget);
        evaluating_ = nullptr;
        evaluatingBudget_ = nullptr;
        if (closed_ || RetireIfCancelled(entry))
            return;
        if (!report_.firstAdvisoryFailure) {
            auto failure = entry.executor->TakeProviderFailure(tick);
            if (closed_ || RetireIfCancelled(entry))
                return;
            if (failure && failure->asset.IsValid() && failure->node.IsValid()) {
                report_.firstAdvisoryFailure.emplace(
                    AiSchedulingAdvisoryFailure{std::move(failure->error),
                                                {entry.agent, entry.identity, AiSchedulingFailurePhase::Evaluate, failure->asset,
                                                 failure->node, true}});
            }
        }
        if (evaluated.HasError() || evaluated.Value() >= AiSliceOutcome::Count) {
            ++report_.failed;
            if (!report_.firstFailure) {
                report_.firstFailure = evaluated.HasError() ? evaluated.ErrorValue() : MakeError(AIErrors::SchedulerPolicyInvalid);
                report_.firstFailureOrigin = AiSchedulingFailureOrigin{entry.agent, entry.identity, AiSchedulingFailurePhase::Evaluate};
            }
            entry.executor->Cancel();
            return;
        }
        entry.intents = true;
        entry.intentSince = tick;
        const auto outcome = evaluated.Value();
        const bool exhausted = budget.Exhausted() || outcome == AiSliceOutcome::BudgetExhausted;
        if (exhausted)
            ++report_.exhausted;
        if (outcome != AiSliceOutcome::Complete || exhausted) {
            ++report_.yielded;
            const bool retry = entry.policy.intervalTicks == 0 ? exhausted : !settings_.simulationProfile;
            if (retry) {
                entry.pending.explicitRequest = true;
                entry.waitingSince = tick;
            }
        }
    }

    /** @copydoc AiTaskScheduler::ValidateDecisionTick */
    Result<void> AiTaskScheduler::ValidateDecisionTick(const std::uint64_t tick) const {
        if (closed_ || inPhase_ || tick == 0 || tick < report_.tick || (tick == report_.tick && committed_))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (settings_.mode != AiSchedulingMode::Deterministic || tick == report_.tick || report_.tick == 0)
            return Result<void>::Success();
        if (tick != report_.tick + 1 || !committed_)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPhaseInvalid));
        if (std::ranges::any_of(entries_, [](const Entry &entry) {
            return entry.intents && !entry.cancellation.IsCancellationRequested();
        }))
            return Result<void>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
        return Result<void>::Success();
    }

    /** @copydoc AiTaskScheduler::EvaluateAtDecision */
    Result<AiSchedulingReport> AiTaskScheduler::EvaluateAtDecision(const std::uint64_t tick) {
        return EvaluateAtDecisionWithPausedAgent(tick, {});
    }

    /** @copydoc AiTaskScheduler::EvaluateReadyEntry */
    void AiTaskScheduler::EvaluateReadyEntry(Entry &entry, const std::uint64_t tick, const AgentHandle pausedAgent) {
        if (RetireIfCancelled(entry) || entry.agent == pausedAgent || entry.tier == AiSimulationTier::Suspended || !entry.pending.Any())
            return;
        if (tick - entry.waitingSince >= settings_.starvationTicks)
            ++report_.starved;
        if (entry.intents || entry.evaluatedTick == tick || report_.evaluated == settings_.evaluationsPerTick ||
            report_.workUnits == settings_.workUnitsPerTick) {
            ++report_.deferred;
            return;
        }
        EvaluateEntry(entry);
    }

    /** @copydoc AiTaskScheduler::EvaluateAtDecisionWithPausedAgent */
    Result<AiSchedulingReport> AiTaskScheduler::EvaluateAtDecisionWithPausedAgent(const std::uint64_t tick, const AgentHandle pausedAgent) {
        if (const auto validated = ValidateDecisionTick(tick); validated.HasError())
            return Result<AiSchedulingReport>::Failure(validated.ErrorValue());
        const bool newTick = tick != report_.tick;
        if (newTick) {
            report_ = {.tick = tick};
            committed_ = false;
            for (auto &entry : entries_) {
                entry.submitted = 0;
                entry.commands = 0;
            }
        }
        inPhase_ = true;
        if (newTick && settings_.simulationProfile)
            ApplyRelevance();
        if (!closed_) {
            jobs_->Pump();
            Order();
        }
        report_.deferred = 0;
        report_.starved = 0;
        for (const auto index : order_) {
            if (closed_)
                break;
            EvaluateReadyEntry(entries_[index], tick, pausedAgent);
        }
        inPhase_ = false;
        if (closed_)
            Shutdown();
        return Result<AiSchedulingReport>::Success(report_);
    }

    /** @copydoc AiTaskScheduler::CommitEntry */
    void AiTaskScheduler::CommitEntry(Entry &entry) {
        const auto units = std::min(entry.policy.commandsPerTick - entry.commands, settings_.commandsPerTick - report_.commands);
        AiWorkBudget budget{units};
        // Reserve the finite command slice before callbacks, including cancellation or zero-work yield.
        entry.commands += units;
        report_.commands += units;
        const auto committed = entry.executor->Commit(entry.context, budget);
        if (closed_)
            return;
        if (RetireIfCancelled(entry))
            return;
        if (committed.HasError()) {
            ++report_.failed;
            if (!report_.firstFailure) {
                report_.firstFailure = committed.ErrorValue();
                report_.firstFailureOrigin = AiSchedulingFailureOrigin{entry.agent, entry.identity, AiSchedulingFailurePhase::Commit};
            }
            entry.executor->Cancel();
            entry.intents = false;
        } else {
            entry.intents = !committed.Value();
            if (entry.intents)
                ++report_.exhausted;
        }
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
            if (RetireIfCancelled(entry))
                continue;
            if (!entry.intents || entry.commands == entry.policy.commandsPerTick || report_.commands == settings_.commandsPerTick)
                continue;
            CommitEntry(entry);
        }
        committed_ = true;
        inPhase_ = false;
        if (closed_)
            Shutdown();
        return Result<AiSchedulingReport>::Success(report_);
    }

    /** @copydoc AiTaskScheduler::CompletedReportAtSafePoint */
    const AiSchedulingReport *AiTaskScheduler::CompletedReportAtSafePoint() const noexcept {
        return committed_ && !inPhase_ && !closed_ ? &report_ : nullptr;
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
        activeByHandle_.clear();
    }

    /** @copydoc AiTaskScheduler::~AiTaskScheduler */
    AiTaskScheduler::~AiTaskScheduler() {
        Shutdown();
    }
}  // namespace Horo::AI
