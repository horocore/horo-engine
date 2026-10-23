#include "RenderResourceRegistry.h"

#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "RenderExternalImages.h"
#include "RenderFrontendErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
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

    RenderResourceRegistry::RenderResourceRegistry(const RenderResourceOwnerId owner, const RenderResourceRegistryLimits &limits,
                                                   BackendResourceRelease releaseBackendResource, RenderMemoryBudget *memoryBudget)
        : owner_(owner), limits_(limits), slots_(owner, limits.maximumSlots), retirementQueue_(limits.maximumSlots),
          releaseBackendResource_(std::move(releaseBackendResource)), memoryBudget_(memoryBudget) {
        assert(owner_.IsValid());
        assert(limits_.IsValid());
        slots_.PrepareCapacity();
        submissionPins_.reserve(limits_.maximumSubmissionPins);
        queueProgress_.reserve(limits_.maximumTrackedQueues);
    }

    /** @copydoc RenderResourceRegistry::SlotStore::PrepareCapacity */
    void RenderResourceRegistry::SlotStore::PrepareCapacity() {
        entries_.reserve(static_cast<std::size_t>(maximumSlots_) + 1);
        freeSlots_.reserve(maximumSlots_);
    }

    /** @copydoc RenderResourceRegistry::SlotStore::ReturnUnpublished */
    void RenderResourceRegistry::SlotStore::ReturnUnpublished(const std::size_t slot) noexcept {
        assert(slot != 0 && slot < entries_.size());
        freeSlots_.emplace_back(static_cast<std::uint32_t>(slot));
    }

    /** @copydoc RenderResourceRegistry::SlotStore::RecycleTerminal */
    void RenderResourceRegistry::SlotStore::RecycleTerminal(const std::size_t slot) noexcept {
        Entry &entry = entries_[slot];
        assert(entry.state == RenderResourceState::Retired);
        assert(!entry.nativeLease.claimed || entry.nativeLease.released);
        if (entry.generation == std::numeric_limits<std::uint32_t>::max()) {
            entry.generationExhausted = true;
        } else {
            ++entry.generation;
            freeSlots_.emplace_back(static_cast<std::uint32_t>(slot));
        }
    }

    /** @copydoc RenderResourceRegistry::OperationJournal::PreparePending */
    RenderResourceRegistry::OperationRecord RenderResourceRegistry::OperationJournal::PreparePending() const {
        OperationRecord record{.id = ResourceOperationId{nextIdentity_}};
        record.error = std::make_unique<OperationDiagnostic>(FrontendErrors::ResourceRegistryStopped,
                                                             "The pending resource operation was cancelled by frontend shutdown.");
        record.rollbackError = std::make_unique<OperationDiagnostic>(FrontendErrors::ResourceOperationCancelled,
                                                                     "The pending resource operation was cancelled before realization.");
        return record;
    }

    /** @copydoc RenderResourceRegistry::OperationJournal::AppendPending */
    ResourceOperationId RenderResourceRegistry::OperationJournal::AppendPending(OperationRecord record) {
        assert(record.id.value == nextIdentity_ && CanIssue());
        records_.push_back(std::move(record));
        return ResourceOperationId{nextIdentity_++};
    }

    /** @copydoc RenderResourceRegistry::OperationJournal::CompleteRollback */
    void RenderResourceRegistry::OperationJournal::CompleteRollback(const ResourceOperationId id) noexcept {
        const auto record = std::ranges::find(records_, id, &OperationRecord::id);
        assert(record != records_.end() && record->rollbackError);
        Complete(id, std::move(record->rollbackError));
    }

    /** @copydoc RenderResourceRegistry::OperationJournal::CompleteShutdown */
    void RenderResourceRegistry::OperationJournal::CompleteShutdown(const ResourceOperationId id) noexcept {
        const auto record = std::ranges::find(records_, id, &OperationRecord::id);
        assert(record != records_.end() && record->error);
        record->complete = true;
        record->rollbackError.reset();
    }

    RenderResourceRegistry::~RenderResourceRegistry() {
        if (HasPendingNativeRetirements())
            std::terminate();
    }

    bool RenderResourceRegistry::HasPendingNativeRetirements() const noexcept {
        return std::ranges::any_of(std::views::iota(std::size_t{0}, slots_.Size()), [this](const std::size_t slot) {
            const auto &entry = slots_.At(slot);
            return entry.nativeLease.claimed && !entry.nativeLease.released;
        });
    }

    bool RenderResourceRegistry::HasNativeLifetime(const RenderResourceIdentity identity) const noexcept {
        const auto *entry = slots_.FindExact(identity);
        return entry != nullptr && entry->nativeLease.claimed;
    }

    Result<RenderResourceNativeLifetime> RenderResourceRegistry::PrepareNativeLifetime(const RenderResourceClass resourceClass,
                                                                                       const RenderResourceIdentity identity,
                                                                                       const RenderMemoryReservationId memoryReservation,
                                                                                       RenderMemoryBudget *memoryBudget) {
        auto slot = slots_.Validate(resourceClass, identity);
        if (slot.HasError())
            return Result<RenderResourceNativeLifetime>::Failure(std::move(slot).ErrorValue());
        auto &entry = slots_.At(slot.Value());
        if (entry.state != RenderResourceState::Pending || entry.nativeLease.claimed)
            return Result<RenderResourceNativeLifetime>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending,
                              "Native lifetime transfer requires one exact unclaimed pending generation."));
        if (memoryBudget != nullptr && memoryBudget_ != nullptr && memoryBudget_ != memoryBudget)
            return Result<RenderResourceNativeLifetime>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Native creation cannot change its registry memory ledger."));
        if (memoryBudget != nullptr)
            memoryBudget_ = memoryBudget;
        entry.memoryReservation = memoryReservation;
        if (!memoryReservation.IsValid())
            return Result<RenderResourceNativeLifetime>::Success({});
        return Result<RenderResourceNativeLifetime>::Success(RenderResourceNativeLifetime{entry.nativeLease});
    }

    Result<void> RenderResourceRegistry::AttachNativeCreation(const RenderResourceClass resourceClass,
                                                              const RenderResourceIdentity identity, const std::uint64_t backendInstance,
                                                              const std::optional<RenderMemoryAllocationId> allocation) {
        auto slot = slots_.Validate(resourceClass, identity);
        if (slot.HasError())
            return Result<void>::Failure(std::move(slot).ErrorValue());
        auto &entry = slots_.At(slot.Value());
        if (entry.state != RenderResourceState::Pending || backendInstance == 0 ||
            (entry.backendInstance != 0 && entry.backendInstance != backendInstance))
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Native creation ownership requires its exact pending generation."));
        entry.backendInstance = backendInstance;
        entry.memoryAllocation = allocation;
        if (allocation.has_value())
            entry.memoryReservation = {};
        return Result<void>::Success();
    }

    Result<ResourceReservation> RenderResourceRegistry::Reserve(const RenderResourceClass resourceClass,
                                                                const std::span<const RenderResourceIdentity> dependencies) {
        if (const Result<void> admitted = ValidateReservationAdmission(); admitted.HasError()) {
            return Result<ResourceReservation>::Failure(admitted.ErrorValue());
        }
        if (const Result<void> validDependencies = slots_.ValidateDependencies(dependencies); validDependencies.HasError()) {
            return Result<ResourceReservation>::Failure(validDependencies.ErrorValue());
        }
        if (const Result<void> resultCapacity = journal_.EnsureCapacity(limits_.maximumOperationResults); resultCapacity.HasError()) {
            return Result<ResourceReservation>::Failure(resultCapacity.ErrorValue());
        }
        std::vector<RenderResourceIdentity> ownedDependencies(dependencies.begin(), dependencies.end());
        auto operation = journal_.PreparePending();
        auto acquiredSlot = slots_.Acquire();
        if (acquiredSlot.HasError()) {
            return Result<ResourceReservation>::Failure(acquiredSlot.ErrorValue());
        }
        const std::size_t slot = acquiredSlot.Value();
        ResourceOperationId operationId;
        try {
            operationId = journal_.AppendPending(std::move(operation));
        } catch (const std::bad_alloc &) {
            slots_.ReturnUnpublished(slot);
            throw;
        } catch (const std::length_error &) {
            slots_.ReturnUnpublished(slot);
            throw;
        }
        Entry &entry = slots_.At(slot);
        entry.resourceClass = resourceClass;
        entry.state = RenderResourceState::Pending;
        entry.dependentPins = 0;
        entry.submissionPins = 0;
        entry.backendInstance = 0;
        entry.memoryAllocation.reset();
        entry.memoryReservation = {};
        entry.nativeLease = {};
        entry.nativeDestroyRequested = false;
        entry.memoryRetirementBegun = false;
        entry.operation = operationId;
        entry.dependencies = std::move(ownedDependencies);
        entry.retirementQueued = false;
        for (const RenderResourceIdentity dependency : dependencies) {
            ++slots_.At(dependency.slot).dependentPins;
        }
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
        if (!journal_.CanIssue()) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource operation identity space is exhausted."));
        }
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::SlotStore::ValidateDependencies(const std::span<const RenderResourceIdentity> dependencies) const {
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

    Result<void> RenderResourceRegistry::OperationJournal::EnsureCapacity(const std::uint32_t maximumResults) {
        if (records_.size() < maximumResults) {
            return Result<void>::Success();
        }
        if (!records_.front().complete) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceQueueFull, "The bounded renderer resource result store is full."));
        }
        records_.pop_front();
        return Result<void>::Success();
    }

    Result<std::size_t> RenderResourceRegistry::SlotStore::Acquire() {
        if (!freeSlots_.empty()) {
            const std::uint32_t slot = freeSlots_.back();
            freeSlots_.pop_back();
            return Result<std::size_t>::Success(slot);
        }
        if (entries_.size() - 1 >= maximumSlots_) {
            return Result<std::size_t>::Failure(
                RegistryError(FrontendErrors::ResourceCapacityExhausted, "The renderer resource slot pool is exhausted."));
        }
        entries_.emplace_back();
        return Result<std::size_t>::Success(entries_.size() - 1);
    }

    Result<void> RenderResourceRegistry::Publish(const RenderResourceClass resourceClass, const RenderResourceIdentity identity,
                                                 const std::uint64_t backendInstance,
                                                 const std::optional<RenderMemoryAllocationId> memoryAllocation) {
        auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = slots_.At(validated.Value());
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
        if (memoryAllocation.has_value())
            entry.memoryReservation = {};
        entry.state = RenderResourceState::Ready;
        --pendingRequests_;
        journal_.Complete(entry.operation, {});
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::Fail(const RenderResourceClass resourceClass, const RenderResourceIdentity identity, Error error) {
        using enum RenderResourceState;

        auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = slots_.At(validated.Value());
        if (entry.state != Pending) {
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Only a pending resource generation can fail creation."));
        }
        auto ownedError = std::make_unique<OperationDiagnostic>(error);
        entry.state = Failed;
        QueueRetirementIfEligible(identity.slot);
        --pendingRequests_;
        journal_.Complete(entry.operation, std::move(ownedError));
        return Result<void>::Success();
    }

    Result<void> RenderResourceRegistry::Release(const RenderResourceClass resourceClass, const RenderResourceIdentity identity) {
        using enum RenderResourceState;

        auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<void>::Failure(validated.ErrorValue());
        }
        Entry &entry = slots_.At(validated.Value());
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
        auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        Entry &entry = slots_.At(validated.Value());
        if (entry.state != RenderResourceState::Pending)
            return Result<void>::Failure(
                RegistryError(FrontendErrors::ResourceNotPending, "Only a pending resource operation can be cancelled."));
        auto cancelled = std::make_unique<OperationDiagnostic>(FrontendErrors::ResourceOperationCancelled,
                                                               "The pending resource operation was cancelled before realization.");
        --pendingRequests_;
        journal_.Complete(entry.operation, std::move(cancelled));
        entry.state = RenderResourceState::Retiring;
        QueueRetirementIfEligible(identity.slot);
        return Result<void>::Success();
    }

    Result<RenderResourceState> RenderResourceRegistry::State(const RenderResourceClass resourceClass,
                                                              const RenderResourceIdentity identity) const {
        auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<RenderResourceState>::Failure(validated.ErrorValue());
        }
        return Result<RenderResourceState>::Success(slots_.At(validated.Value()).state);
    }

    /** @copydoc RenderResourceRegistry::RollbackPending */
    void RenderResourceRegistry::RollbackPending(const RenderResourceClass resourceClass, const ResourceReservation reservation) noexcept {
        if (const Entry *found = slots_.FindExact(reservation.identity); found == nullptr || found->state != RenderResourceState::Pending ||
                                                                         found->resourceClass != resourceClass ||
                                                                         found->operation != reservation.operation)
            return;
        journal_.CompleteRollback(reservation.operation);
        --pendingRequests_;
        slots_.At(reservation.identity.slot).state = RenderResourceState::Retiring;
        QueueRetirementIfEligible(reservation.identity.slot);
    }

    Result<std::uint64_t> RenderResourceRegistry::BackendInstance(const RenderResourceClass resourceClass,
                                                                  const RenderResourceIdentity identity) const {
        const auto validated = slots_.Validate(resourceClass, identity);
        if (validated.HasError()) {
            return Result<std::uint64_t>::Failure(validated.ErrorValue());
        }
        const Entry &entry = slots_.At(validated.Value());
        if (entry.state != RenderResourceState::Ready) {
            return Result<std::uint64_t>::Failure(
                RegistryError(FrontendErrors::ResourceNotReady, "The renderer resource has no usable backend instance."));
        }
        if (entry.externalSource) {
            if (auto current = entry.externalSource->Validate(); current.HasError())
                return Result<std::uint64_t>::Failure(current.ErrorValue());
        }
        return Result<std::uint64_t>::Success(entry.backendInstance);
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
            queueProgress_.emplace_back(QueueProgress{.queue = completion.queue});
            queue = queueProgress_.end() - 1;
        }
        queue->submitted = completion.value;
        submissionPins_.emplace_back(resourceClass, identity, completion);
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

    std::size_t RenderResourceRegistry::DrainRetirements(const BackendResourceReleaseMode releaseMode) noexcept {
        std::size_t retired = 0;
        // Retiring a child can enqueue its now-unpinned parents. Consume those within the same
        // fixed work budget; a still-live native lease may requeue but cannot cause an unbounded scan.
        for (std::size_t examined = 0; examined < limits_.retirementDrainBudget && retirementQueueCount_ != 0; ++examined) {
            const std::uint32_t slot = retirementQueue_[retirementQueueHead_];
            retirementQueueHead_ = (retirementQueueHead_ + 1) % retirementQueue_.size();
            --retirementQueueCount_;
            slots_.At(slot).retirementQueued = false;
            if (Retire(slot, releaseMode))
                ++retired;
        }
        return retired;
    }

    void RenderResourceRegistry::Shutdown(const BackendResourceReleaseMode releaseMode) noexcept {
        using enum RenderResourceState;

        if (!acceptingRequests_) {
            while (DrainRetirements(releaseMode) != 0) {
                // A bounded pass may expose a dependency-free generation for the next pass.
            }
            return;
        }
        acceptingRequests_ = false;
        pendingRequests_ = 0;
        submissionPins_.clear();
        activeSubmissionPins_ = 0;
        queueProgress_.clear();
        completionScanCursor_ = 0;
        for (std::size_t slot = 1; slot < slots_.Size(); ++slot) {
            Entry &entry = slots_.At(slot);
            if (entry.state == Pending) {
                journal_.CompleteShutdown(entry.operation);
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
        for (std::size_t slot = 1; slot < slots_.Size(); ++slot) {
            if (slots_.At(slot).state != Retired) {
                (void)Retire(slot, releaseMode);
            }
        }
        if (!HasPendingNativeRetirements()) {
            retirementQueueHead_ = 0;
            retirementQueueCount_ = 0;
        }
    }

    RenderResourceOwnerId RenderResourceRegistry::Owner() const noexcept {
        return owner_;
    }

    Result<std::size_t> RenderResourceRegistry::SlotStore::Validate(const RenderResourceClass resourceClass,
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

    const RenderResourceRegistry::Entry *RenderResourceRegistry::SlotStore::FindExact(
        const RenderResourceIdentity identity) const noexcept {
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

    void RenderResourceRegistry::OperationJournal::Complete(const ResourceOperationId operationId,
                                                            std::unique_ptr<OperationDiagnostic> error) noexcept {
        const auto operation = std::ranges::find(records_, operationId, &OperationRecord::id);
        assert(operation != records_.end());
        operation->complete = true;
        operation->error = std::move(error);
        operation->rollbackError.reset();
    }

    void RenderResourceRegistry::QueueRetirementIfEligible(const std::size_t slot) {
        Entry &entry = slots_.At(slot);
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

    bool RenderResourceRegistry::Retire(const std::size_t slot, const BackendResourceReleaseMode releaseMode) noexcept {
        Entry &entry = slots_.At(slot);
        if (entry.backendInstance != 0 && !entry.nativeDestroyRequested && releaseBackendResource_) {
            releaseBackendResource_(entry.resourceClass, entry.backendInstance, entry.memoryAllocation, releaseMode);
            entry.nativeDestroyRequested = true;
        }
        if (memoryBudget_ != nullptr && entry.memoryAllocation.has_value() &&
            releaseMode != BackendResourceReleaseMode::NativeUnavailable && !entry.memoryRetirementBegun) {
            if (memoryBudget_->BeginRetire(*entry.memoryAllocation).HasError())
                std::terminate();
            entry.memoryRetirementBegun = true;
        }
        if (entry.nativeLease.claimed && !entry.nativeLease.released) {
            QueueRetirementIfEligible(slot);
            return false;
        }
        if (memoryBudget_ != nullptr) {
            if (entry.memoryReservation.IsValid()) {
                if (memoryBudget_->Cancel(entry.memoryReservation).HasError())
                    std::terminate();
            } else if (entry.memoryAllocation.has_value() && releaseMode != BackendResourceReleaseMode::NativeUnavailable &&
                       memoryBudget_->AcknowledgeRetirement(*entry.memoryAllocation).HasError()) {
                std::terminate();
            }
        }
        for (const RenderResourceIdentity dependency : entry.dependencies) {
            if (slots_.FindExact(dependency) != nullptr) {
                Entry &dependencyEntry = slots_.At(dependency.slot);
                assert(dependencyEntry.dependentPins > 0);
                --dependencyEntry.dependentPins;
                QueueRetirementIfEligible(dependency.slot);
            }
        }
        entry.dependencies.clear();
        entry.backendInstance = 0;
        entry.externalSource.reset();
        entry.memoryAllocation.reset();
        entry.operation = {};
        entry.state = RenderResourceState::Retired;
        entry.retirementQueued = false;
        slots_.RecycleTerminal(slot);
        return true;
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
