#include "NavigationCoordinatorState.h"

#include <tuple>

namespace Horo::Navigation {
    /** @copydoc NavigationCoordinator::State::State */
    NavigationCoordinator::State::State(JobSystem &scheduler, NavigationPathBatchLimits bounds)
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

    /** @copydoc NavigationCoordinator::State::Select */
    std::optional<std::uint32_t> NavigationCoordinator::State::Select(const std::uint64_t now, const std::uint64_t previousCaller,
                                                                      const Batch &staged) const noexcept {
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
            const auto quota = std::ranges::find(quotas, entry.submission.caller.owner, &Quota::caller);
            std::uint64_t callerNodes = quota == quotas.end() ? 0 : quota->nodes;
            std::uint64_t stagedNodes{};
            for (const auto &work : staged.work) {
                stagedNodes += work.query.request.requirement.limits.maximumNodeExpansions;
                if (work.caller.owner == entry.submission.caller.owner)
                    callerNodes += work.query.request.requirement.limits.maximumNodeExpansions;
            }
            const auto requested = entry.submission.request.requirement.limits.maximumNodeExpansions;
            if (requested > limits.nodeExpansionsPerCaller - callerNodes || requested > limits.nodeExpansionsPerTick - nodes - stagedNodes)
                continue;
            if (!selected) {
                selected = index;
                continue;
            }
            const auto &prior = *slots[*selected].entry;
            const auto caller = entry.submission.caller.owner.Value();
            const auto priorCaller = prior.submission.caller.owner.Value();
            const auto rank = std::pair{caller <= previousCaller, caller};
            const auto priorRank = std::pair{priorCaller <= previousCaller, priorCaller};
            if (rank < priorRank) {
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
        const auto quota = std::ranges::find(quotas, caller, &Quota::caller);
        if (quota == quotas.end())
            quotas.push_back({caller, 1, requestedNodes});
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
            auto completion = Complete(work, 0, std::move(result));
            if (state->completions.TryPush(completion) != NavigationQueueEnqueueResult::Enqueued)
                batch.fallback[workIndex].emplace(std::move(completion));
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
            // A later exception must not replace an earlier successful result with the partition failure.
            CollectCompletions();
            // Terminal jobs have returned from the callback or revoked queued work. Fallback buffers are now owner-only.
            for (std::size_t index = 0; index < batch.work.size(); ++index) {
                auto *entry = Find(batch.work[index].query.handle);
                if (!entry)
                    continue;
                entry->executing = false;
                if (!entry->published && !entry->candidate) {
                    if (batch.fallback[index]) {
                        batch.fallback[index]->job = entry->job;
                        entry->candidate.emplace(std::move(*batch.fallback[index]));
                    } else if (snapshot->state == JobState::Cancelled || snapshot->state == JobState::Failed) {
                        entry->candidate.emplace(Complete(batch.work[index], entry->job,
                                                          Result<NavigationPath>::Failure(snapshot->state == JobState::Cancelled
                                                                                              ? MakeError(NavigationErrors::QueryCancelled)
                                                                                              : snapshot->error.value_or(MakeError(
                                                                                                    NavigationErrors::ProviderFailed)))));
                    }
                }
                Retire(slots[batch.work[index].query.handle.slot.index]);
            }
            batch.job.reset();
            batch.work.clear();
            for (auto &fallback : batch.fallback)
                fallback.reset();
        }
    }
}  // namespace Horo::Navigation
