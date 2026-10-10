#include "NavigationCoordinatorState.h"

#include <bit>
#include <cmath>
#include <new>
#include <ranges>

namespace Horo::Navigation {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Reject malformed owned path descriptors before acquiring result storage. */
        bool ValidSubmission(const NavigationPathSubmission &input, const std::uint64_t tick) noexcept {
            const auto &request = input.request;
            return input.caller.owner.IsValid() && input.caller.generation.IsValid() && ValidateNavigationOutcomeProvenance(input.source) &&
                   input.source.world == request.world && input.source.topology == request.topology && Math::IsFinite(request.start) &&
                   Math::IsFinite(request.destination) && request.filter.IsValid() &&
                   request.coveragePolicy < NavigationPathCoveragePolicy::Count && request.requirement.query == NavigationQueryKind::Path &&
                   input.priority <= JobPriority::Background && input.targetTick >= tick && input.deadlineTick >= input.targetTick &&
                   input.deadlineTick > tick && std::isfinite(request.clearanceMeters) && request.clearanceMeters >= 0.0F;
        }
    }  // namespace

    /** @copydoc NavigationCoordinator::Create */
    Result<NavigationCoordinator> NavigationCoordinator::Create(JobSystem &jobs, NavigationPathBatchLimits limits) {
        if (!std::has_single_bit(limits.requestSlots) || limits.requestSlots < 2 || limits.requestSlots > 4096 || limits.maximumJobs == 0 ||
            limits.maximumJobs > limits.requestSlots || limits.requestsPerJob == 0 || limits.requestsPerJob > limits.requestSlots ||
            limits.maximumJobs > limits.requestSlots / limits.requestsPerJob || limits.requestsPerTick == 0 ||
            limits.requestsPerTick > limits.requestSlots || limits.requestsPerCaller == 0 ||
            limits.requestsPerCaller > limits.requestsPerTick || limits.maximumPendingPerCaller == 0 ||
            limits.maximumPendingPerCaller >= limits.requestSlots || limits.nodeExpansionsPerCaller == 0 ||
            limits.nodeExpansionsPerCaller >= limits.nodeExpansionsPerTick)
            return Failure<NavigationCoordinator>(NavigationErrors::CapabilityDescriptorInvalid);
        const auto queueBytes = Detail::BoundedMpmcQueue<NavigationPathCompletion>::StorageBytes(limits.requestSlots);
        const std::size_t ownedBytes =
            *queueBytes + sizeof(State) + limits.requestSlots * (sizeof(State::Slot) + sizeof(State::Quota) + sizeof(std::uint32_t)) +
            limits.maximumJobs *
                (sizeof(State::Batch) + limits.requestsPerJob * (sizeof(State::Work) + sizeof(std::optional<NavigationPathCompletion>)));
        if (ownedBytes > limits.maximumOwnedBytes)
            return Failure<NavigationCoordinator>(NavigationErrors::CapacityExceeded);
        try {
            return Result<NavigationCoordinator>::Success(NavigationCoordinator{std::make_shared<State>(jobs, limits)});
        } catch (const std::bad_alloc &) {
            return Failure<NavigationCoordinator>(NavigationErrors::CapacityExceeded);
        }
    }

    NavigationCoordinator::NavigationCoordinator(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc NavigationCoordinator::NavigationCoordinator(NavigationCoordinator&&) */
    NavigationCoordinator::NavigationCoordinator(NavigationCoordinator &&other) noexcept = default;

    /** @copydoc NavigationCoordinator::~NavigationCoordinator */
    NavigationCoordinator::~NavigationCoordinator() {
        BeginShutdown();
    }

    /** @copydoc NavigationCoordinator::Submit */
    Result<NavRequestHandle> NavigationCoordinator::Submit(NavigationWorldLifecycle &world, NavigationPathSubmission input,
                                                           const std::uint64_t tick) {
        if (!state_ || state_->closed)
            return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
        if (tick < state_->tick || !ValidSubmission(input, tick))
            return Failure<NavRequestHandle>(NavigationErrors::CapabilityDescriptorInvalid);
        if (input.request.requirement.limits.maximumNodeExpansions > state_->limits.nodeExpansionsPerCaller)
            return Failure<NavRequestHandle>(NavigationErrors::QueryLimitExceeded);
        state_->Collect();
        const auto outstanding = std::ranges::count_if(state_->slots, [&](const State::Slot &slot) {
            return slot.entry && slot.entry->submission.caller.owner == input.caller.owner;
        });
        if (outstanding >= state_->limits.maximumPendingPerCaller)
            return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
        auto lease = world.Acquire(input.request.world);
        if (lease.HasError())
            return Result<NavRequestHandle>::Failure(std::move(lease).ErrorValue());
        if (lease.Value().Descriptor().topology != input.request.topology)
            return Failure<NavRequestHandle>(NavigationErrors::StaleSnapshot);
        const auto admission =
            AdmitNavigationQuery(lease.Value().Backend().Capabilities(), input.capabilityRevision, input.request.requirement);
        if (admission.HasError())
            return Result<NavRequestHandle>::Failure(admission.ErrorValue());
        for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
            auto &slot = state_->slots[index];
            if (slot.entry || slot.generation == 0)
                continue;
            if (state_->sequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<NavRequestHandle>(NavigationErrors::GenerationExhausted);
            const NavRequestHandle handle{input.request.world, {index, slot.generation}};
            try {
                CancellationSource cancellation{input.cancellation};
                NavigationQueuedQuery query{state_->sequence + 1, handle,         input.request, std::move(lease).Value(),
                                            cancellation.Token(), input.operation};
                slot.entry.emplace(State::Entry{.submission = std::move(input),
                                                .cancellation = std::move(cancellation),
                                                .query = std::move(query),
                                                .admittedTick = tick});
            } catch (const std::bad_alloc &) {
                return Failure<NavRequestHandle>(NavigationErrors::CapacityExceeded);
            }
            ++state_->sequence;
            state_->tick = tick;
            return Result<NavRequestHandle>::Success(handle);
        }
        return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
    }

    /** @copydoc NavigationCoordinator::Cancel */
    bool NavigationCoordinator::Cancel(const NavRequestHandle handle) noexcept {
        auto *entry = state_ ? state_->Find(handle) : nullptr;
        if (!entry || entry->published)
            return false;
        entry->cancellation.RequestCancellation();
        return true;
    }

    /** @copydoc NavigationCoordinator::Dispatch */
    std::uint32_t NavigationCoordinator::Dispatch(const std::uint64_t tick) {
        if (!state_ || state_->closed || tick < state_->tick)
            return 0;
        state_->tick = tick;
        state_->Collect();
        if (state_->dispatchTick != tick) {
            state_->dispatchTick = tick;
            state_->dispatched = 0;
            state_->nodes = 0;
            state_->quotas.clear();
        }
        const auto before = state_->dispatched;
        for (std::uint32_t index = 0; index < state_->batches.size() && state_->dispatched < state_->limits.requestsPerTick; ++index) {
            auto &batch = state_->batches[index];
            if (batch.job)
                continue;
            auto caller = state_->lastCaller;
            while (batch.work.size() < state_->limits.requestsPerJob &&
                   state_->dispatched + batch.work.size() < state_->limits.requestsPerTick) {
                const auto selected = state_->Select(tick, caller, batch);
                if (!selected)
                    break;
                const auto &entry = *state_->slots[*selected].entry;
                const auto activePartitions = std::ranges::count_if(state_->batches, [&](const State::Batch &active) {
                    return active.job && !active.work.empty() && active.work.front().query.request.world == entry.query.request.world;
                });
                if (activePartitions >= entry.query.worldLease.Backend().Capabilities().maximumConcurrentQueries)
                    break;
                if (!batch.work.empty() && !state_->Compatible(batch.work.front(), entry))
                    break;
                batch.work.push_back({entry.query, entry.submission.caller, entry.submission.source, entry.submission.configuration});
                caller = entry.submission.caller.owner.Value();
            }
            if (batch.work.empty())
                continue;
            const auto &first = *state_->Find(batch.work.front().query.handle);
            JobDescriptor descriptor{.configuration = first.submission.configuration, .priority = first.submission.priority};
            // Owner submission must never wait, even if a host accidentally configures a blocking queue.
            JobProducerScope producer{JobProducerRole::MainEditor};
            auto accepted = state_->jobs.SubmitResult(descriptor, [state = state_, index](const CancellationToken &token) {
                return State::Execute(state, index, token);
            });
            if (accepted.HasError()) {
                batch.work.clear();
                break;  // Foundation pressure defers accepted navigation work until a later bounded attempt/deadline.
            }
            batch.job.emplace(std::move(accepted).Value());
            for (const auto &work : batch.work) {
                auto &entry = *state_->Find(work.query.handle);
                entry.executing = true;
                entry.job = batch.job->Id();
                state_->Charge(work.caller.owner, work.query.request.requirement.limits.maximumNodeExpansions);
                ++state_->dispatched;
            }
            state_->lastCaller = caller;
        }
        return state_->dispatched - before;
    }

    /** @copydoc NavigationCoordinator::Commit */
    std::uint32_t NavigationCoordinator::Commit(const NavigationPathPublication &current) {
        const bool active = current.activation.IsValid();
        const bool empty = !current.activation.scene.IsValid() && !current.activation.sceneGeneration.IsValid() &&
                           !current.activation.world.IsValid() && !current.activation.topology.IsValid();
        if (!state_ || current.tick < state_->tick || (!active && !empty) ||
            (active && (!ValidateNavigationOutcomeProvenance(current.source) || current.source.world != current.activation.world ||
                        current.source.topology != current.activation.topology)) ||
            current.callers.size() > state_->limits.requestSlots)
            return 0;
        state_->tick = current.tick;
        state_->Collect();
        auto &order = state_->publicationOrder;
        order.clear();
        for (std::uint32_t index = 0; index < state_->slots.size(); ++index) {
            if (state_->slots[index].entry && !state_->slots[index].entry->published)
                order.push_back(index);
        }
        std::ranges::sort(order, {}, [&](const std::uint32_t index) {
            return state_->slots[index].entry->query.acceptedSequence;
        });
        std::uint32_t published{};
        for (const auto index : order) {
            auto &entry = *state_->slots[index].entry;
            std::optional<Error> failure;
            if (entry.cancellation.Token().IsCancellationRequested())
                failure = MakeError(NavigationErrors::QueryCancelled);
            else if (!active || entry.query.worldLease.IsRevoked() ||
                     entry.query.worldLease.Descriptor().scene != current.activation.scene ||
                     entry.query.worldLease.Descriptor().sceneGeneration != current.activation.sceneGeneration ||
                     entry.query.request.world != current.activation.world)
                failure = MakeError(NavigationErrors::InvalidWorld);
            else if (std::ranges::find(current.callers, entry.submission.caller) == current.callers.end())
                failure = MakeError(NavigationErrors::InvalidHandle);
            else if (!State::SameSource(entry.submission.source, current.source))
                failure = MakeError(NavigationErrors::StaleSnapshot);
            else if (current.tick > entry.submission.deadlineTick)
                failure = MakeError(NavigationErrors::CapacityExceeded);
            if (failure)
                entry.cancellation.RequestCancellation();
            if (!failure && entry.submission.targetTick > current.tick)
                continue;
            if (!failure && !entry.candidate)
                break;  // Worker arrival cannot bypass an earlier eligible admitted request.
            if (failure) {
                entry.terminal.emplace(NavigationPathCompletion{entry.query.handle, entry.submission.caller, entry.query.acceptedSequence,
                                                                entry.job, entry.submission.source, entry.query.worldLease.Descriptor(),
                                                                Result<NavigationPath>::Failure(std::move(*failure))});
                entry.candidate.reset();
            } else {
                entry.terminal.emplace(std::move(*entry.candidate));
                entry.candidate.reset();
            }
            entry.terminal->source.completionTick = current.tick;
            entry.published = true;
            ++published;
        }
        return published;
    }

    /** @copydoc NavigationCoordinator::Take */
    std::optional<NavigationPathCompletion> NavigationCoordinator::Take(const NavRequestHandle handle) {
        if (!state_)
            return std::nullopt;
        state_->Collect();
        auto *entry = state_->Find(handle);
        if (!entry || !entry->terminal || entry->taken)
            return std::nullopt;
        auto result = std::move(entry->terminal);
        entry->terminal.reset();
        entry->taken = true;
        state_->Retire(state_->slots[handle.slot.index]);
        return result;
    }

    /** @copydoc NavigationCoordinator::BeginShutdown */
    void NavigationCoordinator::BeginShutdown() noexcept {
        if (!state_)
            return;
        state_->closed = true;
        for (auto &slot : state_->slots) {
            if (slot.entry && !slot.entry->published)
                slot.entry->cancellation.RequestCancellation();
        }
        for (auto &batch : state_->batches) {
            if (batch.job)
                static_cast<void>(batch.job->RequestCancel());
        }
    }

    /** @copydoc NavigationCoordinator::IsDrained */
    bool NavigationCoordinator::IsDrained() const noexcept {
        return !state_ || (std::ranges::none_of(state_->slots, [](const State::Slot &slot) {
            return slot.entry.has_value();
        }) && std::ranges::none_of(state_->batches, [](const State::Batch &batch) {
            return batch.job.has_value();
        }));
    }
}  // namespace Horo::Navigation
