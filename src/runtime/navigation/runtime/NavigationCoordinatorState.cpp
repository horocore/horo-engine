#include "NavigationCoordinatorState.h"

#include <tuple>

namespace Horo::Navigation {
    /** @copydoc NavigationCoordinator::State::State */
    NavigationCoordinator::State::State(JobSystem &scheduler, const NavigationPathBatchLimits &bounds)
        : jobs(scheduler), limits(bounds), slots(bounds.requestSlots), batches(bounds.maximumJobs), completions(bounds.requestSlots) {
        quotas.reserve(bounds.requestSlots);
        publicationOrder.reserve(bounds.requestSlots);
        for (auto &batch : batches) {
            batch.work.reserve(bounds.requestsPerJob);
            batch.fallback.resize(bounds.requestsPerJob);
        }
    }

    /** @copydoc NavigationCoordinator::State::Find */
    NavigationCoordinator::State::Entry *NavigationCoordinator::State::Find(const NavRequestHandle handle) noexcept {
        if (!handle.IsValid() || handle.slot.index >= slots.size())
            return nullptr;
        auto &slot = slots[handle.slot.index];
        return slot.entry && slot.generation == handle.slot.generation && slot.entry->query.handle == handle ? &*slot.entry : nullptr;
    }

    /** @copydoc NavigationCoordinator::State::SameSource */
    bool NavigationCoordinator::State::SameSource(const NavigationOutcomeProvenance &a, const NavigationOutcomeProvenance &b) noexcept {
        return a.snapshot == b.snapshot && a.world == b.world && a.topology == b.topology && a.obstacleRevision == b.obstacleRevision &&
               a.filterRevision == b.filterRevision && a.profileRevision == b.profileRevision && a.originRevision == b.originRevision;
    }

    /** @copydoc NavigationCoordinator::State::Compatible */
    bool NavigationCoordinator::State::Compatible(const Work &work, const Entry &entry) const noexcept {
        return work.query.worldLease.Descriptor() == entry.query.worldLease.Descriptor() &&
               SameSource(work.source, entry.submission.source) && work.query.request.filter == entry.query.request.filter &&
               !entry.submission.configuration && !work.configuration &&
               entry.submission.priority == slots[work.query.handle.slot.index].entry->submission.priority;
    }

    /** @copydoc NavigationCoordinator::State::Used */
    std::uint32_t NavigationCoordinator::State::Used(const NavigationDynamicOwnerId caller, const Batch &staged) const noexcept {
        const auto quota = std::ranges::find(quotas, caller, &Quota::caller);
        const auto prior = quota == quotas.end() ? 0U : quota->dispatched;
        return prior + static_cast<std::uint32_t>(std::ranges::count(staged.work, caller, [](const Work &work) {
            return work.caller.owner;
        }));
    }

    /** @copydoc NavigationCoordinator::State::WithinNodeBudget */
    bool NavigationCoordinator::State::WithinNodeBudget(const Entry &entry, const Batch &staged) const noexcept {
        const auto quota = std::ranges::find(quotas, entry.submission.caller.owner, &Quota::caller);
        std::uint64_t callerNodes = quota == quotas.end() ? 0 : quota->nodes;
        std::uint64_t stagedNodes{};
        for (const auto &work : staged.work) {
            stagedNodes += work.query.request.requirement.limits.maximumNodeExpansions;
            if (work.caller.owner == entry.submission.caller.owner)
                callerNodes += work.query.request.requirement.limits.maximumNodeExpansions;
        }
        const auto requested = entry.submission.request.requirement.limits.maximumNodeExpansions;
        return requested <= limits.nodeExpansionsPerCaller - callerNodes && requested <= limits.nodeExpansionsPerTick - nodes - stagedNodes;
    }

    /** @copydoc NavigationCoordinator::State::PrepareBatch */
    void NavigationCoordinator::State::PrepareBatch(const std::uint32_t index, const std::uint64_t now) {
        auto &batch = batches[index];
        auto caller = lastCaller;
        while (batch.work.size() < limits.requestsPerJob && dispatched + batch.work.size() < limits.requestsPerTick) {
            if (!StageNext(batch, now, caller))
                break;
        }
    }

    /** @copydoc NavigationCoordinator::State::StageNext */
    bool NavigationCoordinator::State::StageNext(Batch &batch, const std::uint64_t now, std::uint64_t &caller) {
        const auto selected = Select(now, caller, batch);
        if (!selected.has_value())
            return false;
        const auto &entry = *slots[*selected].entry;
        if (const auto activePartitions = std::ranges::count_if(batches,
                                                                [&](const Batch &active) {
            return active.job && !active.work.empty() && active.work.front().query.request.world == entry.query.request.world;
        });
            activePartitions >= entry.query.worldLease.Backend().Capabilities().maximumConcurrentQueries)
            return false;
        if (!batch.work.empty() && !Compatible(batch.work.front(), entry))
            return false;
        batch.work.emplace_back(entry.query, entry.submission.caller, entry.submission.source, entry.submission.configuration);
        caller = entry.submission.caller.owner.Value();
        return true;
    }

    /** @copydoc NavigationCoordinator::State::SubmitBatch */
    bool NavigationCoordinator::State::SubmitBatch(const std::shared_ptr<State> &state, const std::uint32_t index) {
        auto &batch = state->batches[index];
        const auto &first = *state->Find(batch.work.front().query.handle);
        JobDescriptor descriptor{.configuration = first.submission.configuration, .priority = first.submission.priority};
        // Owner submission must never wait, even if a host accidentally configures a blocking queue.
        JobProducerScope producer{JobProducerRole::MainEditor};
        auto accepted = state->jobs.SubmitResult(descriptor, [state, index](const CancellationToken &token) {
            return Execute(state, index, token);
        });
        if (accepted.HasError()) {
            batch.work.clear();
            return false;
        }
        batch.job.emplace(std::move(accepted).Value());
        for (const auto &work : batch.work) {
            auto &entry = *state->Find(work.query.handle);
            entry.executing = true;
            entry.job = batch.job->Id();
            state->Charge(work.caller.owner, work.query.request.requirement.limits.maximumNodeExpansions);
            ++state->dispatched;
        }
        state->lastCaller = batch.work.back().caller.owner.Value();
        return true;
    }

    /** @copydoc NavigationCoordinator::State::ValidPublication */
    bool NavigationCoordinator::State::ValidPublication(const NavigationPathPublication &current) const noexcept {
        const bool active = current.activation.IsValid();
        if (const bool empty = !current.activation.scene.IsValid() && !current.activation.sceneGeneration.IsValid() &&
                               !current.activation.world.IsValid() && !current.activation.topology.IsValid();
            current.tick < tick || (!active && !empty) || current.callers.size() > limits.requestSlots ||
            (active && (!ValidateNavigationOutcomeProvenance(current.source) || current.source.world != current.activation.world ||
                        current.source.topology != current.activation.topology)))
            return false;
        NavigationDynamicOwnerId previous;
        for (const auto &caller : current.callers) {
            if (!caller.owner.IsValid() || !caller.generation.IsValid() || caller.owner <= previous)
                return false;
            previous = caller.owner;
        }
        return true;
    }

    /** @copydoc NavigationCoordinator::State::PublicationFailure */
    std::optional<Error> NavigationCoordinator::State::PublicationFailure(const Entry &entry, const NavigationPathPublication &current) {
        if (entry.cancellation.Token().IsCancellationRequested())
            return MakeError(NavigationErrors::QueryCancelled);
        if (!current.activation.IsValid() || entry.query.worldLease.IsRevoked() ||
            entry.query.worldLease.Descriptor().scene != current.activation.scene ||
            entry.query.worldLease.Descriptor().sceneGeneration != current.activation.sceneGeneration ||
            entry.query.request.world != current.activation.world)
            return MakeError(NavigationErrors::InvalidWorld);
        if (const auto caller = std::ranges::lower_bound(current.callers, entry.submission.caller.owner, {}, &NavigationPathCaller::owner);
            caller == current.callers.end() || *caller != entry.submission.caller)
            return MakeError(NavigationErrors::InvalidHandle);
        if (!SameSource(entry.submission.source, current.source))
            return MakeError(NavigationErrors::StaleSnapshot);
        if (current.tick > entry.submission.deadlineTick)
            return MakeError(NavigationErrors::CapacityExceeded);
        return std::nullopt;
    }

    /** @copydoc NavigationCoordinator::State::Publish */
    void NavigationCoordinator::State::Publish(Entry &entry, std::optional<Error> failure, const std::uint64_t now) {
        if (failure.has_value()) {
            entry.cancellation.RequestCancellation();
            entry.terminal.emplace(entry.query.handle, entry.submission.caller, entry.query.acceptedSequence, entry.job,
                                   entry.submission.source, entry.query.worldLease.Descriptor(),
                                   Result<NavigationPath>::Failure(std::move(*failure)));
        } else {
            entry.terminal.emplace(std::move(*entry.candidate));
        }
        entry.candidate.reset();
        entry.terminal->source.completionTick = now;
        entry.published = true;
    }

    /** @copydoc NavigationCoordinator::State::Select */
    std::optional<std::uint32_t> NavigationCoordinator::State::Select(const std::uint64_t now, const std::uint64_t previousCaller,
                                                                      const Batch &staged) noexcept {
        if (limits.selectionProbesPerTick - selectionProbes < slots.size())
            return std::nullopt;
        selectionProbes += static_cast<std::uint32_t>(slots.size());
        std::optional<std::uint32_t> selected;
        for (std::uint32_t index = 0; index < slots.size(); ++index) {
            const auto &slot = slots[index];
            if (!slot.entry)
                continue;
            const auto &entry = *slot.entry;
            if (entry.job != 0 || entry.published || entry.cancellation.Token().IsCancellationRequested() ||
                entry.query.worldLease.IsRevoked() || now > entry.submission.deadlineTick ||
                Used(entry.submission.caller.owner, staged) >= limits.requestsPerCaller ||
                std::ranges::any_of(staged.work, [&](const Work &work) {
                return work.query.handle == entry.query.handle;
            }))
                continue;
            if (!WithinNodeBudget(entry, staged))
                continue;
            if (!selected.has_value()) {
                selected = index;
                continue;
            }
            const auto &prior = *slots[*selected].entry;
            const auto caller = entry.submission.caller.owner.Value();
            const auto priorCaller = prior.submission.caller.owner.Value();
            const auto rank = std::pair{caller <= previousCaller, caller};
            if (const auto priorRank = std::pair{priorCaller <= previousCaller, priorCaller}; rank < priorRank) {
                selected = index;
                continue;
            }
            if (caller != priorCaller)
                continue;
            const auto requestRank = [&](const Entry &value) {
                const bool aged = now - value.admittedTick >= limits.priorityAgingTicks;
                return std::tuple{!aged, aged ? JobPriority::Interactive : value.submission.priority,
                                  aged ? value.query.acceptedSequence : value.submission.deadlineTick, value.query.acceptedSequence};
            };
            if (requestRank(entry) < requestRank(prior))
                selected = index;
        }
        return selected;
    }

    /** @copydoc NavigationCoordinator::State::Charge */
    void NavigationCoordinator::State::Charge(const NavigationDynamicOwnerId caller, const std::uint32_t requestedNodes) {
        if (const auto quota = std::ranges::find(quotas, caller, &Quota::caller); quota == quotas.end())
            quotas.emplace_back(caller, 1, requestedNodes);
        else {
            ++quota->dispatched;
            quota->nodes += requestedNodes;
        }
        nodes += requestedNodes;
    }

    /** @copydoc NavigationCoordinator::State::Retire */
    void NavigationCoordinator::State::Retire(Slot &slot) noexcept {
        if (slot.entry && slot.entry->taken && !slot.entry->executing) {
            slot.entry.reset();
            if (slot.generation != std::numeric_limits<std::uint32_t>::max())
                ++slot.generation;
            else
                slot.generation = 0;  // Permanently retire exhausted identities; never wrap into an issued generation.
        }
    }

    /** @copydoc NavigationCoordinator::State::Complete */
    NavigationPathCompletion NavigationCoordinator::State::Complete(const Work &work, const JobId job, Result<NavigationPath> result) {
        return {work.query.handle, work.caller, work.query.acceptedSequence, job, work.source, work.query.worldLease.Descriptor(),
                std::move(result)};
    }

    /** @copydoc NavigationCoordinator::State::ValidResult */
    bool NavigationCoordinator::State::ValidResult(const NavigationPathRequest &request, const NavigationPath &path) noexcept {
        const auto points = request.requirement.limits.maximumResultPoints;
        const auto output = request.outputLimits;
        return path.sourceGeneration == request.topology && path.status < NavigationPathStatus::Count &&
               (path.status != NavigationPathStatus::Partial || request.coveragePolicy == NavigationPathCoveragePolicy::AllowPartial) &&
               path.points.size() <= points &&
               path.corridor.size() <=
                   (output.maximumCorridorPolygons ? output.maximumCorridorPolygons : request.requirement.limits.maximumNodeExpansions) &&
               path.portals.size() <= (output.maximumPortals ? output.maximumPortals : points) &&
               path.waypoints.size() <= (output.maximumWaypoints ? output.maximumWaypoints : points);
    }

    /** @copydoc NavigationCoordinator::State::Execute */
    Result<void> NavigationCoordinator::State::Execute(const std::shared_ptr<State> &state, const std::uint32_t index,
                                                       const CancellationToken &cancellation) {
        auto &batch = state->batches[index];
        for (std::size_t workIndex = 0; workIndex < batch.work.size(); ++workIndex) {
            const auto &work = batch.work[workIndex];
            Telemetry::ScopedOperationContext context{work.query.operation};
            auto result = Result<NavigationPath>::Failure(MakeError(NavigationErrors::QueryCancelled));
            if (!cancellation.IsCancellationRequested() && !work.query.requestCancellation.IsCancellationRequested() &&
                !work.query.worldLease.IsRevoked()) {
                result = work.query.worldLease.Backend().FindPath(work.query.request, work.query.requestCancellation);
                if (result.HasValue() && !ValidResult(work.query.request, result.Value()))
                    result = Result<NavigationPath>::Failure(MakeError(NavigationErrors::ProviderFailed));
            }
            if (auto completion = Complete(work, 0, std::move(result));
                state->completions.TryPush(completion) != NavigationQueueEnqueueResult::Enqueued)
                batch.fallback[workIndex].emplace(std::move(completion));
            ++batch.produced;
        }
        return Result<void>::Success();
    }

    /** @copydoc NavigationCoordinator::State::CollectCompletions */
    void NavigationCoordinator::State::CollectCompletions() {
        while (auto completion = completions.TryPop()) {
            if (auto *entry = Find(completion->handle); entry && !entry->published && !entry->candidate) {
                completion->job = entry->job;
                entry->candidate.emplace(std::move(*completion));
            }
        }
    }

    /** @copydoc NavigationCoordinator::State::CollectBatchWork */
    void NavigationCoordinator::State::CollectBatchWork(Batch &batch, const JobSnapshot &snapshot) {
        using enum JobState;
        for (std::size_t index = 0; index < batch.work.size(); ++index) {
            auto *entry = Find(batch.work[index].query.handle);
            if (!entry)
                continue;
            entry->executing = false;
            if (entry->published || entry->candidate) {
                Retire(slots[batch.work[index].query.handle.slot.index]);
                continue;
            }
            if (batch.fallback[index]) {
                batch.fallback[index]->job = entry->job;
                entry->candidate.emplace(std::move(*batch.fallback[index]));
            } else if (index >= batch.produced && (snapshot.state == Cancelled || snapshot.state == Failed)) {
                entry->candidate.emplace(
                    Complete(batch.work[index], entry->job,
                             Result<NavigationPath>::Failure(snapshot.state == Cancelled
                                                                 ? MakeError(NavigationErrors::QueryCancelled)
                                                                 : snapshot.error.value_or(MakeError(NavigationErrors::ProviderFailed)))));
            }
            Retire(slots[batch.work[index].query.handle.slot.index]);
        }
    }

    /** @copydoc NavigationCoordinator::State::Collect */
    void NavigationCoordinator::State::Collect() {
        CollectCompletions();
        for (auto &batch : batches) {
            if (!batch.job)
                continue;
            const auto snapshot = batch.job->Snapshot();
            if (!snapshot || !snapshot->terminalResult)
                continue;
            // Reacquire records published between the initial drain and observing the terminal fence.
            // A later exception must not replace an earlier produced result, even behind another producer's reservation hole.
            CollectCompletions();
            // Terminal jobs have returned from the callback or revoked queued work. Fallback buffers are now owner-only.
            CollectBatchWork(batch, *snapshot);
            batch.job.reset();
            batch.work.clear();
            batch.produced = 0;
            for (auto &fallback : batch.fallback)
                fallback.reset();
        }
    }
}  // namespace Horo::Navigation
