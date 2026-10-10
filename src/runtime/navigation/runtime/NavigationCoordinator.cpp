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
            limits.nodeExpansionsPerCaller >= limits.nodeExpansionsPerTick || limits.selectionProbesPerTick < limits.requestSlots ||
            limits.selectionProbesPerTick > MaximumNavigationPathSelectionProbes)
            return Failure<NavigationCoordinator>(NavigationErrors::CapabilityDescriptorInvalid);
        const auto queueBytes = Detail::BoundedMpmcQueue<NavigationPathCompletion>::StorageBytes(limits.requestSlots);
        if (!queueBytes.has_value())
            return Failure<NavigationCoordinator>(NavigationErrors::CapacityExceeded);
        if (const std::size_t ownedBytes =
                *queueBytes + sizeof(State) + limits.requestSlots * (sizeof(State::Slot) + sizeof(State::Quota) + sizeof(std::uint32_t)) +
                limits.maximumJobs * (sizeof(State::Batch) +
                                      limits.requestsPerJob * (sizeof(State::Work) + sizeof(std::optional<NavigationPathCompletion>)));
            ownedBytes > limits.maximumOwnedBytes)
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
    Result<NavRequestHandle> NavigationCoordinator::Submit(const NavigationWorldLifecycle &world, NavigationPathSubmission input,
                                                           const std::uint64_t tick) {
        const auto &state = MutableState();
        if (!state || state->closed)
            return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
        if (tick < state->tick || !ValidSubmission(input, tick))
            return Failure<NavRequestHandle>(NavigationErrors::CapabilityDescriptorInvalid);
        if (input.request.requirement.limits.maximumNodeExpansions > state->limits.nodeExpansionsPerCaller)
            return Failure<NavRequestHandle>(NavigationErrors::QueryLimitExceeded);
        state->Collect();
        if (const auto outstanding = std::ranges::count_if(state->slots,
                                                           [&](const State::Slot &slot) {
            return slot.entry && slot.entry->submission.caller.owner == input.caller.owner;
        });
            outstanding >= state->limits.maximumPendingPerCaller)
            return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
        auto lease = world.Acquire(input.request.world);
        if (lease.HasError())
            return Result<NavRequestHandle>::Failure(std::move(lease).ErrorValue());
        if (lease.Value().Descriptor().topology != input.request.topology)
            return Failure<NavRequestHandle>(NavigationErrors::StaleSnapshot);
        if (const auto admission =
                AdmitNavigationQuery(lease.Value().Backend().Capabilities(), input.capabilityRevision, input.request.requirement);
            admission.HasError())
            return Result<NavRequestHandle>::Failure(admission.ErrorValue());
        for (std::uint32_t index = 0; index < state->slots.size(); ++index) {
            auto &slot = state->slots[index];
            if (slot.entry || slot.generation == 0)
                continue;
            if (state->sequence == std::numeric_limits<std::uint64_t>::max())
                return Failure<NavRequestHandle>(NavigationErrors::GenerationExhausted);
            const NavRequestHandle handle{input.request.world, {index, slot.generation}};
            try {
                CancellationSource cancellation{input.cancellation};
                NavigationQueuedQuery query{state->sequence + 1,  handle,         input.request, std::move(lease).Value(),
                                            cancellation.Token(), input.operation};
                slot.entry.emplace(State::Entry{.submission = std::move(input),
                                                .cancellation = std::move(cancellation),
                                                .query = std::move(query),
                                                .admittedTick = tick});
            } catch (const std::bad_alloc &) {
                return Failure<NavRequestHandle>(NavigationErrors::CapacityExceeded);
            }
            ++state->sequence;
            state->tick = tick;
            return Result<NavRequestHandle>::Success(handle);
        }
        return Failure<NavRequestHandle>(NavigationErrors::AdmissionRejected);
    }

    /** @copydoc NavigationCoordinator::Cancel */
    bool NavigationCoordinator::Cancel(const NavRequestHandle handle) noexcept {
        const auto &state = MutableState();
        const auto *entry = state ? state->Find(handle) : nullptr;
        if (!entry || entry->published)
            return false;
        entry->cancellation.RequestCancellation();
        return true;
    }

    /** @copydoc NavigationCoordinator::Dispatch */
    std::uint32_t NavigationCoordinator::Dispatch(const std::uint64_t tick) {
        const auto &state = MutableState();
        if (!state || state->closed || tick < state->tick)
            return 0;
        state->tick = tick;
        state->Collect();
        if (state->dispatchTick != tick) {
            state->dispatchTick = tick;
            state->dispatched = 0;
            state->nodes = 0;
            state->selectionProbes = 0;
            state->quotas.clear();
        }
        const auto before = state->dispatched;
        for (std::uint32_t index = 0; index < state->batches.size() && state->dispatched < state->limits.requestsPerTick; ++index) {
            const auto &batch = state->batches[index];
            if (batch.job)
                continue;
            state->PrepareBatch(index, tick);
            if (!batch.work.empty() && !State::SubmitBatch(state, index))
                break;  // Foundation pressure retains accepted navigation requests for a later bounded attempt.
        }
        return state->dispatched - before;
    }

    /** @copydoc NavigationCoordinator::Commit */
    std::uint32_t NavigationCoordinator::Commit(const NavigationPathPublication &current) {
        const auto &state = MutableState();
        if (!state || !state->ValidPublication(current))
            return 0;
        state->tick = current.tick;
        state->Collect();
        auto &order = state->publicationOrder;
        order.clear();
        for (std::uint32_t index = 0; index < state->slots.size(); ++index) {
            if (state->slots[index].entry && !state->slots[index].entry->published)
                order.push_back(index);
        }
        std::ranges::sort(order, {}, [&](const std::uint32_t index) {
            return state->slots[index].entry->query.acceptedSequence;
        });
        std::uint32_t published{};
        for (const auto index : order) {
            auto &entry = *state->slots[index].entry;
            auto failure = State::PublicationFailure(entry, current);
            if (!failure.has_value() && entry.submission.targetTick > current.tick)
                continue;
            if (!failure.has_value() && !entry.candidate)
                break;  // Worker arrival cannot bypass an earlier eligible admitted request.
            State::Publish(entry, std::move(failure), current.tick);
            ++published;
        }
        return published;
    }

    /** @copydoc NavigationCoordinator::Take */
    std::optional<NavigationPathCompletion> NavigationCoordinator::Take(const NavRequestHandle handle) {
        const auto &state = MutableState();
        if (!state)
            return std::nullopt;
        state->Collect();
        auto *entry = state->Find(handle);
        if (!entry || !entry->terminal || entry->taken)
            return std::nullopt;
        auto result = std::move(entry->terminal);
        entry->terminal.reset();
        entry->taken = true;
        State::Retire(state->slots[handle.slot.index]);
        return result;
    }

    /** @copydoc NavigationCoordinator::BeginShutdown */
    void NavigationCoordinator::BeginShutdown() noexcept {
        const auto &state = MutableState();
        if (!state)
            return;
        state->closed = true;
        for (auto &slot : state->slots) {
            if (slot.entry && !slot.entry->published)
                slot.entry->cancellation.RequestCancellation();
        }
        for (auto &batch : state->batches) {
            if (batch.job)
                static_cast<void>(batch.job->RequestCancel());
        }
    }

    /** @copydoc NavigationCoordinator::MutableState */
    std::shared_ptr<NavigationCoordinator::State> &NavigationCoordinator::MutableState() noexcept {
        return state_;
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
