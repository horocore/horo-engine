#include "RenderResourceRegistry.h"

#include "RenderFrontendErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] std::atomic<std::uint64_t> &NextOwnerId() noexcept {
            static std::atomic<std::uint64_t> nextOwnerId{1};
            return nextOwnerId;
        }

        [[nodiscard]] Error RegistryError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    bool RenderResourceRegistryLimits::IsValid() const noexcept {
        return std::min({maximumSlots, maximumPendingRequests, retirementDrainBudget, maximumSubmissionPins, maximumTrackedQueues,
                         completionDrainBudget}) > 0 &&
               maximumOperationResults >= maximumPendingRequests;
    }

    RenderResourceRegistry::RenderResourceRegistry(const RenderResourceOwnerId owner, const RenderResourceRegistryLimits limits,
                                                   BackendResourceRelease releaseBackendResource)
        : owner_(owner), limits_(limits), retirementQueue_(limits.maximumSlots),
          releaseBackendResource_(std::move(releaseBackendResource)) {
        assert(owner_.IsValid());
        assert(limits_.IsValid());
        freeSlots_.reserve(limits_.maximumSlots);
        submissionPins_.reserve(limits_.maximumSubmissionPins);
        queueProgress_.reserve(limits_.maximumTrackedQueues);
    }

    Result<ResourceReservation> RenderResourceRegistry::Reserve(const RenderResourceClass resourceClass,
                                                                const std::span<const RenderResourceIdentity> dependencies) {
        if (const Result<void> admitted = ValidateReservationAdmission(); admitted.HasError()) {
            return Result<ResourceReservation>::Failure(admitted.ErrorValue());
        }
        if (const Result<void> validDependencies = ValidateDependencies(dependencies); validDependencies.HasError()) {
            return Result<ResourceReservation>::Failure(validDependencies.ErrorValue());
        }
        if (const Result<void> resultCapacity = EnsureOperationResultCapacity(); resultCapacity.HasError()) {
            return Result<ResourceReservation>::Failure(resultCapacity.ErrorValue());
        }
        auto acquiredSlot = AcquireSlot();
        if (acquiredSlot.HasError()) {
            return Result<ResourceReservation>::Failure(acquiredSlot.ErrorValue());
        }
        const std::size_t slot = acquiredSlot.Value();
        Entry &entry = entries_[slot];
        entry.resourceClass = resourceClass;
        entry.state = RenderResourceState::Pending;
        entry.dependentPins = 0;
        entry.submissionPins = 0;
        entry.backendInstance = 0;
        entry.memoryAllocation.reset();
        entry.operation = ResourceOperationId{nextOperation_++};
        entry.dependencies.assign(dependencies.begin(), dependencies.end());
        entry.retirementQueued = false;
        for (const RenderResourceIdentity dependency : dependencies) {
            ++entries_[dependency.slot].dependentPins;
        }
        operations_.push_back(OperationRecord{.id = entry.operation});
        ++pendingRequests_;
        return Result<ResourceReservation>::Success(
            ResourceReservation{.identity = {owner_, static_cast<std::uint32_t>(slot), entry.generation}, .operation = entry.operation});
    }

    Result<void> RenderResourceRegistry::ValidateReservationAdmission() const {
        if (!acceptingRequests_) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceRegistryStopped, "Resource creation is disabled during frontend shutdown."));
        }
        if (pendingRequests_ >= limits_.maximumPendingRequests) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceQueueFull, "The bounded renderer resource request queue is full."));
        }
        if (nextOperation_ == 0) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource operation identity space is exhausted."));
        }
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::ValidateDependencies(const std::span<const RenderResourceIdentity> dependencies) const {
        for (std::size_t dependencyIndex = 0; dependencyIndex < dependencies.size(); ++dependencyIndex) {
            const RenderResourceIdentity dependency = dependencies[dependencyIndex];
            if (const auto earlierDependencies = dependencies.first(dependencyIndex);
                std::ranges::find(earlierDependencies, dependency) != earlierDependencies.end()) {
                return Result<void>::Failure(
                    RegistryError(FrontendErrors::ResourceHandleMalformed, "A resource dependency generation is listed more than once."));
            }
            const Entry *entry = FindExact(dependency);
            if (entry == nullptr || entry->state != RenderResourceState::Ready) {
                return Result<void>::Failure(
                    RegistryError(FrontendErrors::ResourceDependencyNotReady, "A resource dependency is foreign, stale, or not ready."));
            }
            if (entry->dependentPins == std::numeric_limits<std::uint32_t>::max()) {
                return Result<void>::Failure(
                    RegistryError(FrontendErrors::ResourceCapacityExhausted, "A renderer resource dependency pin count is exhausted."));
            }
        }
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::EnsureOperationResultCapacity() {
        if (operations_.size() < limits_.maximumOperationResults) {
            return Result<void>::Success();
        }
        if (!operations_.front().complete) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceQueueFull, "The bounded renderer resource result store is full."));
        }
        operations_.pop_front();
        return Result<void>::Success();
    }

    Result<std::size_t> RenderResourceRegistry::AcquireSlot() {
        if (!freeSlots_.empty()) {
            const std::uint32_t slot = freeSlots_.back();
            freeSlots_.pop_back();
            return Result<std::size_t>::Success(slot);
        }
        if (entries_.size() - 1 >= limits_.maximumSlots) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource slot pool is exhausted."));
        }
        entries_.emplace_back();
        return Result<std::size_t>::Success(entries_.size() - 1);
    }

    Result<void> RenderResourceRegistry::Publish(const RenderResourceClass resourceClass, const RenderResourceIdentity identity,
                                                 const std::uint64_t backendInstance,
                                                 const std::optional<RenderMemoryAllocationId> memoryAllocation) {
        auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.state != RenderResourceState::Pending) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Only a pending resource generation can be published."));
        }
        if (backendInstance == 0) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceBackendInstanceInvalid, "A ready resource requires a backend instance identity."));
        }
        entry.backendInstance = backendInstance;
        entry.memoryAllocation = memoryAllocation;
        entry.state = RenderResourceState::Ready;
        --pendingRequests_;
        CompleteOperation(entry.operation, std::nullopt);
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::Fail(const RenderResourceClass resourceClass, const RenderResourceIdentity identity, Error error) {
        using enum RenderResourceState;

        auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.state != Pending) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Only a pending resource generation can fail creation."));
        }
        entry.state = Failed;
        QueueRetirementIfEligible(identity.slot);
        --pendingRequests_;
        CompleteOperation(entry.operation, std::move(error));
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::Release(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
        using enum RenderResourceState;

        auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.state == Retiring) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceAlreadyRetiring, "The resource generation is already retiring."));
        }
        if (entry.state != Ready) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotReady, "Only a ready resource generation can be released."));
        }
        entry.state = Retiring;
        QueueRetirementIfEligible(identity.slot);
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::CancelPending(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
        auto validated = Validate(resourceClass, identity);
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        Entry &entry = entries_[validated.Value()];
        if (entry.state != RenderResourceState::Pending)
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Only a pending resource operation can be cancelled."));
        --pendingRequests_;
        CompleteOperation(entry.operation, RegistryError(FrontendErrors::ResourceOperationCancelled,
                                                         "The pending resource operation was cancelled before realization."));
        entry.state = RenderResourceState::Retiring;
        QueueRetirementIfEligible(identity.slot);
        return Result<void>::Success();
    }

    Result<RenderResourceState> RenderResourceRegistry::State(const RenderResourceClass resourceClass,
                                                              const RenderResourceIdentity identity) const {
        auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<RenderResourceState>::Failure(validated.ErrorValue());
        }
        return Result<RenderResourceState>::Success(entries_[validated.Value()].state);
    }

    Result<void> RenderResourceRegistry::OperationResult(const ResourceOperationId operation) const {
        if (!operation.IsValid()) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceOperationUnknown, "The resource operation identity is invalid."));
        }
        const auto record = std::ranges::find(operations_, operation, &OperationRecord::id);
        if (record == operations_.end()) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceOperationUnknown, "The resource operation does not belong to this registry."));
        }
        if (!record->complete) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceOperationPending, "The renderer resource operation has not completed."));
        }
        if (record->error.has_value()) {
            return Result<void>::Failure(*record->error);
        }
        return Result<void>::Success();
    }

    Result<std::uint64_t> RenderResourceRegistry::BackendInstance(const RenderResourceClass resourceClass,
                                                                  const RenderResourceIdentity identity) const {
        const auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<std::uint64_t>::Failure(validated.ErrorValue());
        }
        const Entry &entry = entries_[validated.Value()];
        if (entry.state != RenderResourceState::Ready) {
            return Result<std::uint64_t>::Failure(
                RegistryError(FrontendErrors::ResourceNotReady, "The renderer resource has no usable backend instance."));
        }
        return Result<std::uint64_t>::Success(entry.backendInstance);
    }

    Result<void> RenderResourceRegistry::AddSubmissionPin(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
        const auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.state != RenderResourceState::Ready) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotReady, "Only a ready resource may enter a new submission."));
        }
        if (entry.submissionPins == std::numeric_limits<std::uint32_t>::max()) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource submission pin count is exhausted."));
        }
        ++entry.submissionPins;
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::ReleaseSubmissionPin(const RenderResourceClass resourceClass,
                                                              const RenderResourceIdentity identity) {
        const auto validated = Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = entries_[validated.Value()];
        if (entry.submissionPins == 0) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceHandleMalformed, "The renderer resource has no submission pin to release."));
        }
        --entry.submissionPins;
        QueueRetirementIfEligible(identity.slot);
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::TrackSubmission(const RenderResourceClass resourceClass, const RenderResourceIdentity identity,
                                                         const RenderTimelinePoint completion) {
        if (!completion.IsValid()) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceCompletionInvalid, "A resource submission requires a valid queue completion point."));
        }
        if (!acceptingRequests_) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceRegistryStopped, "Resource submissions are disabled during frontend shutdown."));
        }
        auto queue = std::ranges::find(queueProgress_, completion.queue, &QueueProgress::queue);
        const bool newQueue = queue == queueProgress_.end();
        if (newQueue && queueProgress_.size() >= limits_.maximumTrackedQueues) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceSubmissionCapacityExceeded,
                              "The renderer cannot track another logical queue without exceeding its configured queue bound."));
        }
        if (!newQueue && (completion.value <= queue->completed || completion.value < queue->submitted)) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceCompletionRegressed,
                              "The resource submission completion point is already complete or regresses its logical queue."));
        }
        if (submissionPins_.size() >= limits_.maximumSubmissionPins) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceSubmissionCapacityExceeded, "The bounded renderer submission-pin queue is full."));
        }
        if (const Result<void> pinned = AddSubmissionPin(resourceClass, identity); pinned.HasError()) {
            return pinned;
        }
        if (newQueue) {
            queueProgress_.push_back(QueueProgress{.queue = completion.queue});
            queue = queueProgress_.end() - 1;
        }
        queue->submitted = std::max(queue->submitted, completion.value);
        submissionPins_.push_back(SubmissionPin{resourceClass, identity, completion});
        return Result<void>::Success();
    }

    Result<std::size_t> RenderResourceRegistry::AcknowledgeCompletion(const RenderTimelinePoint completion) {
        if (!completion.IsValid()) {
            return Result<std::size_t>::Failure(RegistryError(FrontendErrors::ResourceCompletionInvalid,
                                                              "A completion acknowledgement requires a valid queue timeline point."));
        }
        auto queue = std::ranges::find(queueProgress_, completion.queue, &QueueProgress::queue);
        if (queue == queueProgress_.end()) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceCompletionUnknownQueue,
                              "The completion acknowledgement names a queue with no accepted resource submissions."));
        }
        if (completion.value < queue->completed) {
            return Result<std::size_t>::Failure(RegistryError(FrontendErrors::ResourceCompletionRegressed,
                                                              "The completion acknowledgement regresses the queue's completed timeline."));
        }
        queue->completed = completion.value;

        std::size_t released = 0;
        std::size_t inspected = 0;
        while (!submissionPins_.empty() && inspected < limits_.completionDrainBudget) {
            completionScanCursor_ %= submissionPins_.size();
            const SubmissionPin &pin = submissionPins_[completionScanCursor_];
            const auto pinQueue = std::ranges::find(queueProgress_, pin.completion.queue, &QueueProgress::queue);
            const bool complete = pinQueue != queueProgress_.end() && pin.completion.value <= pinQueue->completed;
            ++inspected;
            if (!complete) {
                completionScanCursor_ = (completionScanCursor_ + 1) % submissionPins_.size();
                continue;
            }
            const SubmissionPin completedPin = pin;
            submissionPins_[completionScanCursor_] = submissionPins_.back();
            submissionPins_.pop_back();
            if (const Result<void> unpinned = ReleaseSubmissionPin(completedPin.resourceClass, completedPin.identity);
                unpinned.HasError()) {
                return Result<std::size_t>::Failure(unpinned.ErrorValue());
            }
            ++released;
        }
        if (submissionPins_.empty()) {
            completionScanCursor_ = 0;
        }
        return Result<std::size_t>::Success(released);
    }

    std::size_t RenderResourceRegistry::DrainRetirements(const BackendResourceReleaseMode releaseMode) {
        std::size_t retired = 0;
        while (retired < limits_.retirementDrainBudget && retirementQueueCount_ > 0) {
            const std::uint32_t slot = retirementQueue_[retirementQueueHead_];
            retirementQueueHead_ = (retirementQueueHead_ + 1) % retirementQueue_.size();
            --retirementQueueCount_;
            Retire(slot, releaseMode);
            ++retired;
        }
        return retired;
    }

    void RenderResourceRegistry::Shutdown(const BackendResourceReleaseMode releaseMode) noexcept {
        using enum RenderResourceState;

        if (!acceptingRequests_) {
            return;
        }
        acceptingRequests_ = false;
        pendingRequests_ = 0;
        submissionPins_.clear();
        queueProgress_.clear();
        completionScanCursor_ = 0;
        for (std::size_t slot = 1; slot < entries_.size(); ++slot) {
            Entry &entry = entries_[slot];
            if (entry.state == Pending) {
                CompleteOperation(entry.operation, RegistryError(FrontendErrors::ResourceRegistryStopped,
                                                                 "The pending resource operation was cancelled by frontend shutdown."));
                entry.state = Retiring;
            } else if (entry.state == Ready) {
                entry.state = Retiring;
            }
            entry.submissionPins = 0;
            QueueRetirementIfEligible(slot);
        }
        while (DrainRetirements(releaseMode) != 0) {
            // Continue until dependency retirement makes no further entry eligible.
        }
        for (std::size_t slot = 1; slot < entries_.size(); ++slot) {
            if (entries_[slot].state != Retired) {
                Retire(slot, releaseMode);
            }
        }
        retirementQueueHead_ = 0;
        retirementQueueCount_ = 0;
    }

    RenderResourceOwnerId RenderResourceRegistry::Owner() const noexcept {
        return owner_;
    }

    Result<std::size_t> RenderResourceRegistry::Validate(const RenderResourceClass resourceClass,
                                                         const RenderResourceIdentity identity) const {
        if (const std::array<std::uint64_t, 3> identityFields{identity.owner.value, identity.slot, identity.generation};
            std::ranges::find(identityFields, 0) != identityFields.end()) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceHandleMalformed, "The renderer resource handle contains a zero identity field."));
        }
        if (identity.owner != owner_) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceWrongOwner, "The renderer resource handle belongs to another frontend."));
        }
        if (identity.slot >= entries_.size()) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceSlotOutOfRange, "The renderer resource slot is outside the registry."));
        }
        const Entry &entry = entries_[identity.slot];
        if (entry.generation != identity.generation || entry.state == RenderResourceState::Retired) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceStale, "The renderer resource generation is stale or retired."));
        }
        if (entry.resourceClass != resourceClass) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceWrongType, "The renderer resource handle type does not match the registry entry."));
        }
        return Result<std::size_t>::Success(identity.slot);
    }

    const RenderResourceRegistry::Entry *RenderResourceRegistry::FindExact(const RenderResourceIdentity identity) const noexcept {
        if (identity.owner != owner_) {
            return nullptr;
        }
        if (identity.slot == 0 || identity.slot >= entries_.size()) {
            return nullptr;
        }
        const Entry &entry = entries_[identity.slot];
        if (entry.generation != identity.generation || entry.state == RenderResourceState::Retired) {
            return nullptr;
        }
        return &entry;
    }

    void RenderResourceRegistry::CompleteOperation(const ResourceOperationId operationId, std::optional<Error> error) {
        const auto operation = std::ranges::find(operations_, operationId, &OperationRecord::id);
        assert(operation != operations_.end());
        operation->complete = true;
        operation->error = std::move(error);
    }

    void RenderResourceRegistry::QueueRetirementIfEligible(const std::size_t slot) {
        Entry &entry = entries_[slot];
        const bool retiring = entry.state == RenderResourceState::Retiring || entry.state == RenderResourceState::Failed;
        if (const std::array eligibility{retiring, entry.dependentPins == 0, entry.submissionPins == 0, !entry.retirementQueued};
            !std::ranges::all_of(eligibility, std::identity{})) {
            return;
        }
        assert(retirementQueueCount_ < retirementQueue_.size());
        entry.retirementQueued = true;
        const std::size_t insertion = (retirementQueueHead_ + retirementQueueCount_) % retirementQueue_.size();
        retirementQueue_[insertion] = static_cast<std::uint32_t>(slot);
        ++retirementQueueCount_;
    }

    void RenderResourceRegistry::Retire(const std::size_t slot, const BackendResourceReleaseMode releaseMode) noexcept {
        Entry &entry = entries_[slot];
        for (const RenderResourceIdentity dependency : entry.dependencies) {
            if (FindExact(dependency) != nullptr) {
                Entry &dependencyEntry = entries_[dependency.slot];
                assert(dependencyEntry.dependentPins > 0);
                --dependencyEntry.dependentPins;
                QueueRetirementIfEligible(dependency.slot);
            }
        }
        entry.dependencies.clear();
        if (entry.backendInstance != 0 && releaseBackendResource_) {
            releaseBackendResource_(entry.resourceClass, entry.backendInstance, entry.memoryAllocation, releaseMode);
        }
        entry.backendInstance = 0;
        entry.memoryAllocation.reset();
        entry.operation = {};
        entry.state = RenderResourceState::Retired;
        entry.retirementQueued = false;
        if (entry.generation == std::numeric_limits<std::uint32_t>::max()) {
            entry.generationExhausted = true;
        } else {
            ++entry.generation;
            freeSlots_.push_back(static_cast<std::uint32_t>(slot));
        }
    }

    Result<RenderResourceOwnerId> AcquireRenderResourceOwnerId() {
        std::atomic<std::uint64_t> &nextOwnerId = NextOwnerId();
        std::uint64_t current = nextOwnerId.load();
        while (current != 0) {
            if (nextOwnerId.compare_exchange_weak(current, current + 1)) {
                return Result<RenderResourceOwnerId>::Success(RenderResourceOwnerId{current});
            }
        }
        return Result<RenderResourceOwnerId>::Failure(
            RegistryError(FrontendErrors::ResourceOwnerExhausted, "The process-wide renderer resource owner identity space is exhausted."));
    }
}  // namespace Horo::Render::Detail
