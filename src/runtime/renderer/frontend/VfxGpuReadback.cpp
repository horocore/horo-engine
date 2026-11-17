#include "VfxGpuReadback.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

namespace Horo::Render::Detail {
    /** @copydoc ReserveVfxGpuReadback */
    Result<void> ReserveVfxGpuReadback(VfxGpuStorage &storage, VfxGpuRecord &record, const bool nativeReadbackAvailable) {
        const auto &policy = record.descriptor.readback;
        if (!policy.enabled) {
            record.readback = VfxGpuReadbackAdmission::Disabled;
            return Result<void>::Success();
        }
        const bool slots = storage.reservedReadbackSlots <= storage.readbackPool.slotCount &&
                           policy.maximumPendingResults <= storage.readbackPool.slotCount - storage.reservedReadbackSlots;
        if (const bool bytes =
                std::size_t{policy.maximumRecords} * VfxGpuObservationWords::Bytes <= storage.readbackPool.maximumBytesPerResult;
            !nativeReadbackAvailable || !storage.readbacks || !slots || !bytes) {
            record.readback = VfxGpuReadbackAdmission::OptionalUnavailable;
            if (policy.requirement == VfxGpuReadbackRequirement::RequiredForVisualEffect)
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceUnsupported));
            return Result<void>::Success();
        }
        record.requests.resize(policy.maximumPendingResults);
        storage.readbackOwners.push_back(record.id.slot);
        storage.reservedReadbackSlots += policy.maximumPendingResults;
        record.reservedReadbackSlots = policy.maximumPendingResults;
        record.reservedCpuPayloadBytes = std::size_t{policy.maximumPendingResults} * storage.readbackPool.maximumBytesPerResult;
        record.readback = VfxGpuReadbackAdmission::Available;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseVfxGpuReadbackReservation */
    void ReleaseVfxGpuReadbackReservation(VfxGpuStorage &storage, VfxGpuRecord &record) noexcept {
        if (record.reservedReadbackSlots == 0)
            return;
        if (const bool pending = std::ranges::any_of(record.requests,
                                                     [](const auto &request) {
            return request.occupied;
        });
            pending)
            return;
        storage.reservedReadbackSlots -= record.reservedReadbackSlots;
        record.reservedReadbackSlots = 0;
        record.reservedCpuPayloadBytes = 0;
        std::erase(storage.readbackOwners, record.id.slot);
    }

    /** @copydoc RequestVfxGpuObservation */
    RenderReadbackPreparedStatus RequestVfxGpuObservation(VfxGpuStorage &storage, VfxGpuRecord &record, const VfxGpuStepIdentity step,
                                                          const std::size_t observation, std::size_t &requestIndex) noexcept {
        using Status = RenderReadbackPreparedStatus;
        if (const auto existing = std::ranges::find_if(record.requests,
                                                       [&](const auto &request) {
            return request.occupied && request.step == step;
        });
            existing != record.requests.end()) {
            requestIndex = static_cast<std::size_t>(existing - record.requests.begin());
            return !existing->submitted && !existing->nativeUncertain ? Status::Complete : Status::InvalidTransition;
        }
        const auto free = std::ranges::find_if(record.requests, [](const auto &request) {
            return !request.occupied;
        });
        if (!storage.readbacks || free == record.requests.end())
            return Status::CapacityExceeded;
        const auto bytes = std::size_t{record.descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes;
        const auto now = std::chrono::steady_clock::now();
        if (storage.observationTimeout > std::chrono::steady_clock::time_point::max() - now)
            return Status::InvalidDescriptor;
        const auto requested = storage.readbacks->RequestPrepared({.source = record.observations[observation].handle,
                                                                   .byteCount = bytes,
                                                                   .alignment = storage.readbackPool.alignment,
                                                                   .timeout = storage.observationTimeout});
        if (requested.status != Status::Complete)
            return requested.status;
        requestIndex = static_cast<std::size_t>(free - record.requests.begin());
        *free = {.step = step,
                 .id = requested.id,
                 .deadline = now + storage.observationTimeout,
                 .observation = observation,
                 .occupied = true};
        return Status::Complete;
    }

    /** @copydoc AbandonVfxGpuObservation */
    void AbandonVfxGpuObservation(VfxGpuStorage &storage, VfxGpuRecord &record, const VfxGpuStepIdentity step) noexcept {
        const auto found = std::ranges::find_if(record.requests, [&](const auto &request) {
            return request.occupied && request.step == step;
        });
        if (found == record.requests.end())
            return;
        if (found->submitted || found->nativeUncertain)
            std::terminate();  // This helper is reserved for an independently proven unsubmitted boundary.
        if (storage.readbacks->Cancel(found->id).HasError() || storage.readbacks->Discard(found->id).HasError())
            std::terminate();
        record.observationHeld[found->observation] = 0;
        *found = {};
        static_cast<void>(storage.readbacks->CollectPreparedResults());
    }
}  // namespace Horo::Render::Detail

namespace Horo::Render {
    namespace {
        using Status = RenderReadbackPreparedStatus;
        using namespace Detail;

        Result<std::size_t> NativeCopy(IRenderBackend &backend, const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                       const VfxGpuStepIdentity &step, const std::span<std::byte> destination) {
            try {
                return backend.CopyCompletedVfxGpuObservation(simulation, step, destination);
            } catch (...) {
                return Result<std::size_t>::Failure(MakeError(FrontendErrors::ResourceBackendException));
            }
        }

        Result<void> NativeDiscard(IRenderBackend &backend, const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                   const VfxGpuStepIdentity &step) {
            try {
                return backend.DiscardCompletedVfxGpuObservation(simulation, step);
            } catch (...) {
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceBackendException));
            }
        }

        /** @brief Resolves request-level timeout and cancellation before exposing a result lease. */
        Status ObservationAvailability(const VfxGpuRecord &record, const VfxGpuObservationRequest &request) noexcept {
            using enum Horo::Render::RenderReadbackPreparedStatus;
            if (request.timedOut)
                return TimedOut;
            if (record.released || VfxGpuLifetimeCancelled(record) || request.cancelRequested)
                return Cancelled;
            return Complete;
        }

        void SaturatingAdd(std::uint64_t &counter, const std::uint64_t amount) noexcept {
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            counter = amount > maximum - counter ? maximum : counter + amount;
        }

        std::size_t HeldObservation(const VfxGpuRecord &record, const VfxGpuStepIdentity &step) noexcept {
            for (std::size_t index = 0; index < record.observationSteps.size(); ++index)
                if (record.observationHeld[index] && record.observationSteps[index] == step)
                    return index;
            return record.observationSteps.size();
        }

        bool SamePool(const VfxGpuStorage &storage, const RenderReadbackLimits &limits, const RenderReadbackPreparedStorage &pool,
                      const std::chrono::nanoseconds timeout) noexcept {
            const auto &old = storage.readbackPool;
            const auto &bound = storage.readbackLimits;
            return old.slotCount == pool.slotCount && old.maximumBytesPerResult == pool.maximumBytesPerResult &&
                   old.alignment == pool.alignment && storage.observationTimeout == timeout &&
                   bound.maximumPendingBytes == limits.maximumPendingBytes &&
                   bound.maximumRetainedResultBytes == limits.maximumRetainedResultBytes &&
                   bound.maximumRequestBytes == limits.maximumRequestBytes && bound.maximumAlignment == limits.maximumAlignment &&
                   bound.maximumRequests == limits.maximumRequests;
        }

        bool ValidObservationTimeout(const std::chrono::nanoseconds timeout) noexcept {
            return timeout.count() > 0 && timeout <= std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now();
        }

        Result<std::size_t> ExistingPool(const VfxGpuStorage &storage, const RenderReadbackLimits &limits,
                                         const RenderReadbackPreparedStorage &pool, const std::size_t maximum,
                                         const std::chrono::nanoseconds timeout) {
            return SamePool(storage, limits, pool, timeout) && storage.readbackCpuBytes <= maximum
                       ? Result<std::size_t>::Success(storage.readbackCpuBytes)
                       : Result<std::size_t>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        }

        Result<std::size_t> BuildReadbackPool(VfxGpuStorage &target, const RenderResourceOwnerId owner, const RenderReadbackLimits &limits,
                                              const RenderReadbackPreparedStorage pool, const std::size_t maximumCpuPayloadBytes,
                                              const std::chrono::nanoseconds observationTimeout) {
            using Outcome = Result<std::size_t>;
            auto &storage = target;
            auto queue = RenderReadbackQueue::Create(owner, limits);
            if (queue.HasError())
                return Outcome::Failure(queue.ErrorValue());
            if (pool.slotCount > maximumCpuPayloadBytes / sizeof(std::uint32_t))
                return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
            std::vector<std::uint32_t> owners;
            owners.reserve(pool.slotCount);
            const auto metadata = owners.capacity() * sizeof(std::uint32_t);
            if (metadata > maximumCpuPayloadBytes)
                return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
            const auto prepared = queue.Value()->PreparePayloadStorage(pool, maximumCpuPayloadBytes - metadata);
            if (prepared.HasError())
                return Outcome::Failure(prepared.ErrorValue());

            storage.readbackCpuBytes = prepared.Value() + metadata;
            storage.readbackGlobalRemainderBytes = storage.readbackCpuBytes - std::size_t{pool.slotCount} * pool.maximumBytesPerResult;
            storage.readbackPool = pool;
            storage.readbackLimits = limits;
            storage.observationTimeout = observationTimeout;
            storage.readbackOwners = std::move(owners);
            storage.readbacks = std::move(queue).Value();
            return Outcome::Success(storage.readbackCpuBytes);
        }

        VfxGpuObservationRequest *Observation(VfxGpuRecord &record, const VfxGpuStepIdentity &step) noexcept {
            const auto found = std::ranges::find_if(record.requests, [&](const auto &request) {
                return request.occupied && request.step == step;
            });
            return found == record.requests.end() ? nullptr : std::to_address(found);
        }

        Status FindObservation(VfxGpuStorage *storage, const VfxGpuPreparationReceipt &preparation, const VfxGpuStepIdentity &step,
                               VfxGpuRecord *&record, VfxGpuObservationRequest *&request) noexcept {
            using enum Horo::Render::RenderReadbackPreparedStatus;
            if (!storage || !storage->readbacks || !preparation.IsLive())
                return InvalidRequest;
            record = storage->Find(preparation.Id());
            if (!record)
                return InvalidRequest;
            request = Observation(*record, step);
            return request ? Complete : InvalidRequest;
        }

        bool Copyable(const VfxGpuRecord *record) noexcept {
            return record && record->native && !record->released && !VfxGpuLifetimeCancelled(*record);
        }

        Result<bool> FinishCancelledRequest(RenderReadbackQueue &queue, VfxGpuObservationRequest &request) {
            if (const auto cancelled = request.timedOut && request.submitted ? queue.Timeout(request.id) : queue.Cancel(request.id);
                cancelled.HasError())
                return Result<bool>::Failure(cancelled.ErrorValue());
            if (request.submitted)
                if (const auto retired = queue.Retire(request.id); retired.HasError())
                    return Result<bool>::Failure(retired.ErrorValue());
            if (const auto discarded = queue.Discard(request.id); discarded.HasError())
                return Result<bool>::Failure(discarded.ErrorValue());
            request = {};
            return Result<bool>::Success(true);
        }

        bool Configured(const VfxGpuStorage *storage) noexcept {
            return storage && storage->readbacks;
        }

        bool HasNativeObservation(const VfxGpuRecord &record) noexcept;

        bool FinalizeReadbackOwner(VfxGpuStorage &storage, VfxGpuStorageSlot &slot) noexcept {
            auto &record = *slot.record;
            if (record.released && !HasNativeObservation(record))
                record.native.reset();
            const auto reserved = record.reservedReadbackSlots;
            if (record.released)
                ReleaseVfxGpuReadbackReservation(storage, record);
            if (reserved == record.reservedReadbackSlots)
                return false;
            slot.record.reset();
            ++slot.generation;
            return true;
        }

        bool HasNativeObservation(const VfxGpuRecord &record) noexcept {
            return std::ranges::any_of(record.observationHeld, [](const auto held) {
                return held != 0;
            });
        }
    }  // namespace

    /** @copydoc RenderFrontend::PrepareVfxGpuReadbackStorage */
    Result<std::size_t> RenderFrontend::PrepareVfxGpuReadbackStorage(const RenderReadbackLimits &limits,
                                                                     const RenderReadbackPreparedStorage pool,
                                                                     const std::size_t maximumCpuPayloadBytes,
                                                                     const std::chrono::nanoseconds observationTimeout) {
        using Outcome = Result<std::size_t>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (activeFrameScope_)
            return Outcome::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        if (!ValidObservationTimeout(observationTimeout))
            return Outcome::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (!backend_->Capabilities().support.features.Supports(RenderCapability::VfxGpuReadback))
            return Outcome::Failure(MakeError(FrontendErrors::ResourceUnsupported));
        try {
            if (!vfxGpuStorage_)
                vfxGpuStorage_ = std::make_unique<Detail::VfxGpuStorage>();
            if (vfxGpuStorage_->busy)
                return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
            if (vfxGpuStorage_->readbacks)
                return ExistingPool(*vfxGpuStorage_, limits, pool, maximumCpuPayloadBytes, observationTimeout);
            return BuildReadbackPool(*vfxGpuStorage_, resourceRegistry_->Owner(), limits, pool, maximumCpuPayloadBytes, observationTimeout);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
    }

    /** @copydoc RenderFrontend::ObserveVfxGpuReadbackFact */
    Result<void> RenderFrontend::ObserveVfxGpuReadbackFact(Detail::VfxGpuRecord &record, const NativeGpuVfxStepReceipt &receipt) {
        using enum Horo::Render::VfxGpuSubmissionState;
        auto *request = Observation(record, receipt.step);
        if (!request)
            return Result<void>::Success();
        if (receipt.state == RejectedUnsubmitted || receipt.state == CancelledUnsubmitted) {
            Detail::AbandonVfxGpuObservation(*vfxGpuStorage_, record, receipt.step);
            return Result<void>::Success();
        }
        if (receipt.state == SubmissionUncertain) {
            request->nativeUncertain = true;
            request->cancelRequested = true;
            return Result<void>::Success();
        }
        if (receipt.state == Accepted) {
            if (const auto submitted = vfxGpuStorage_->readbacks->MarkSubmitted(request->id, receipt.submission); submitted.HasError())
                return submitted;
            request->submitted = true;
            request->submission = receipt.submission;
        }
        if (receipt.state == Completed || receipt.state == DeviceLost)
            request->nativeCompleted = true;
        if (receipt.state == DeviceLost)
            request->cancelRequested = true;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::AcquireVfxGpuObservation */
    RenderReadbackPreparedStatus RenderFrontend::AcquireVfxGpuObservation(const VfxGpuPreparationReceipt &preparation,
                                                                          const VfxGpuStepIdentity &step,
                                                                          RenderReadbackResult &result) noexcept {
        using enum Horo::Render::RenderReadbackPreparedStatus;
        if (std::this_thread::get_id() != preparationOwner_)
            return WrongThread;
        if (closing_)
            return Stopped;
        if (vfxGpuStorage_ && vfxGpuStorage_->busy)
            return ResultPending;
        Detail::VfxGpuRecord *record{};
        Detail::VfxGpuObservationRequest *request{};
        if (const auto found = FindObservation(vfxGpuStorage_.get(), preparation, step, record, request); found != Complete)
            return found;
        if (const auto available = ObservationAvailability(*record, *request); available != Complete)
            return available;
        const auto acquired = vfxGpuStorage_->readbacks->AcquirePrepared(request->id, result);
        if (acquired == Complete)
            request->acquired = true;
        return acquired;
    }

    /** @copydoc RenderFrontend::CancelVfxGpuObservation */
    RenderReadbackPreparedStatus RenderFrontend::CancelVfxGpuObservation(const VfxGpuPreparationReceipt &preparation,
                                                                         const VfxGpuStepIdentity &step) noexcept {
        using enum Horo::Render::RenderReadbackPreparedStatus;
        if (std::this_thread::get_id() != preparationOwner_)
            return WrongThread;
        if (closing_)
            return Stopped;
        if (vfxGpuStorage_ && vfxGpuStorage_->busy)
            return ResultPending;
        if (!vfxGpuStorage_ || !vfxGpuStorage_->readbacks || !preparation.IsLive())
            return InvalidRequest;
        auto *record = vfxGpuStorage_->Find(preparation.Id());
        if (!record)
            return InvalidRequest;
        auto *request = Observation(*record, step);
        if (!request)
            return InvalidRequest;
        request->cancelRequested = true;
        return Complete;
    }

    /** @copydoc RenderFrontend::GpuReadbackDiagnostics */
    Result<VfxGpuReadbackDiagnostics> RenderFrontend::GpuReadbackDiagnostics() const {
        using Outcome = Result<VfxGpuReadbackDiagnostics>;
        if (std::this_thread::get_id() != preparationOwner_)
            return Outcome::Failure(MakeError(FrontendErrors::WrongThread));
        if (!vfxGpuStorage_)
            return Outcome::Success({});
        const auto &storage = *vfxGpuStorage_;
        VfxGpuReadbackDiagnostics diagnostics{.preparedCpuBytes = storage.readbackCpuBytes,
                                              .reservedSlots = storage.reservedReadbackSlots,
                                              .observationCopyCalls = storage.observationCopyCalls,
                                              .copiedBytes = storage.copiedObservationBytes,
                                              .observationDiscardCalls = storage.observationDiscardCalls};
        if (storage.readbacks)
            diagnostics.queue = storage.readbacks->Snapshot();
        return Outcome::Success(diagnostics);
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::CopyCompletedVfxGpuObservation */
    Result<std::size_t> Detail::RenderFrontendResourceAccess::CopyCompletedVfxGpuObservation(RenderFrontend &frontend,
                                                                                             const VfxGpuPreparationReceipt &preparation,
                                                                                             const VfxGpuStepIdentity &step,
                                                                                             const std::span<std::byte> destination) {
        using Outcome = Result<std::size_t>;
        if (const auto owner = frontend.RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (frontend.activeFrameScope_ || !frontend.vfxGpuStorage_ || !preparation.IsLive())
            return Outcome::Failure(MakeError(VfxFrameErrors::InvalidInput));
        auto &storage = *frontend.vfxGpuStorage_;
        auto *record = storage.Find(preparation.Id());
        if (!Copyable(record))
            return Outcome::Failure(MakeError(VfxFrameErrors::Cancelled));
        const auto index = HeldObservation(*record, step);
        if (index == record->observationSteps.size() || !record->observationCompleted[index])
            return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
        SaturatingAdd(storage.observationCopyCalls, 1);
        auto copied = NativeCopy(*frontend.backend_, record->native, step, destination);
        if (copied.HasValue()) {
            record->observationHeld[index] = 0;
            SaturatingAdd(storage.copiedObservationBytes, copied.Value());
        }
        return copied;
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::DiscardCompletedVfxGpuObservation */
    Result<void> Detail::RenderFrontendResourceAccess::DiscardCompletedVfxGpuObservation(RenderFrontend &frontend,
                                                                                         const VfxGpuPreparationReceipt &preparation,
                                                                                         const VfxGpuStepIdentity &step) {
        if (const auto owner = frontend.RequireOpenOwner(); owner.HasError())
            return owner;
        if (frontend.activeFrameScope_ || !frontend.vfxGpuStorage_ || !preparation.IsLive())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        auto &storage = *frontend.vfxGpuStorage_;
        auto *record = storage.Find(preparation.Id());
        if (!record || !record->native)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto index = HeldObservation(*record, step);
        if (index == record->observationSteps.size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        SaturatingAdd(storage.observationDiscardCalls, 1);
        auto discarded = NativeDiscard(*frontend.backend_, record->native, step);
        if (discarded.HasValue())
            record->observationHeld[index] = 0;
        return discarded;
    }

    /** @copydoc RenderFrontend::RetireVfxGpuReadbackResult */
    Result<bool> RenderFrontend::RetireVfxGpuReadbackResult(Detail::VfxGpuRecord &record, const std::size_t index) {
        auto &request = record.requests[index];
        const auto retired = vfxGpuStorage_->readbacks->RetirePreparedResult(request.id);
        if (retired == Status::ResultPending)
            return Result<bool>::Success(false);
        if (retired != Status::Complete)
            return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        request = {};
        return Result<bool>::Success(true);
    }

    /** @copydoc RenderFrontend::CancelVfxGpuReadbackRequest */
    Result<bool> RenderFrontend::CancelVfxGpuReadbackRequest(Detail::VfxGpuRecord &record, const std::size_t index) {
        auto &request = record.requests[index];
        auto &queue = *vfxGpuStorage_->readbacks;
        if (request.ready) {
            if (!request.acquired) {
                {  // Consume and drop an unpublished ready lease; this performs no native map or copy.
                    RenderReadbackResult ignored;
                    if (queue.AcquirePrepared(request.id, ignored) != Status::Complete)
                        return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
                }
                request.acquired = true;
            }
            return RetireVfxGpuReadbackResult(record, index);
        }
        if (!request.nativeCompleted && !request.nativeUncertain)
            return Result<bool>::Success(false);
        if (record.observationHeld[request.observation]) {
            if (const auto discarded =
                    Detail::RenderFrontendResourceAccess::DiscardCompletedVfxGpuObservation(*this, IssueVfxGpuReceipt(record),
                                                                                            request.step);
                discarded.HasError()) {
                if (ErrorChainContains(discarded.ErrorValue(), VfxFrameErrors::Pending.domain, VfxFrameErrors::Pending.code))
                    return Result<bool>::Success(false);
                return Result<bool>::Failure(discarded.ErrorValue());
            }
            request.nativeCompleted = true;
        }
        // A pending uncertain request is cancelled only after the native discard just proved terminal completion.
        return FinishCancelledRequest(queue, request);
    }

    /** @copydoc RenderFrontend::CopyVfxGpuObservation */
    Result<bool> RenderFrontend::CopyVfxGpuObservation(Detail::VfxGpuRecord &record, const std::size_t index) {
        auto &request = record.requests[index];
        const auto mapping = vfxGpuStorage_->readbacks->WritablePrepared(request.id);
        if (mapping.status != Status::Complete)
            return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto expected = std::size_t{record.descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes;
        if (mapping.bytes.size() != expected)
            return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto copied = Detail::RenderFrontendResourceAccess::CopyCompletedVfxGpuObservation(*this, IssueVfxGpuReceipt(record),
                                                                                                 request.step, mapping.bytes);
        if (copied.HasError())
            return Result<bool>::Failure(copied.ErrorValue());
        if (copied.Value() != expected)
            return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (std::chrono::steady_clock::now() >= request.deadline) {
            request.cancelRequested = true;
            request.timedOut = true;
            return CancelVfxGpuReadbackRequest(record, index);
        }
        if (vfxGpuStorage_->readbacks->CompletePrepared(request.id, request.submission) != Status::Complete)
            return Result<bool>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        request.ready = true;
        return Result<bool>::Success(true);
    }

    /** @copydoc RenderFrontend::ProcessVfxGpuObservation */
    Result<bool> RenderFrontend::ProcessVfxGpuObservation(Detail::VfxGpuRecord &record, const std::size_t index) {
        auto &request = record.requests[index];
        if (!request.occupied)
            return Result<bool>::Success(false);
        if (request.acquired)
            return RetireVfxGpuReadbackResult(record, index);
        if (!request.ready && std::chrono::steady_clock::now() >= request.deadline) {
            request.cancelRequested = true;
            request.timedOut = true;
        }
        if (record.released || Detail::VfxGpuLifetimeCancelled(record))
            request.cancelRequested = true;
        if (request.cancelRequested)
            return CancelVfxGpuReadbackRequest(record, index);
        if (!request.nativeCompleted || request.ready)
            return Result<bool>::Success(false);
        return CopyVfxGpuObservation(record, index);
    }

    /** @copydoc RenderFrontend::ProcessVfxGpuReadbacks */
    Result<std::size_t> RenderFrontend::ProcessVfxGpuReadbacks() {
        using Outcome = Result<std::size_t>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (activeFrameScope_ || (vfxGpuStorage_ && vfxGpuStorage_->busy))
            return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
        if (!Configured(vfxGpuStorage_.get()))
            return Outcome::Success(0);
        Detail::VfxGpuBusyGuard guard{vfxGpuStorage_->busy};
        auto &storage = *vfxGpuStorage_;
        static_cast<void>(storage.readbacks->CollectPreparedResults());
        std::size_t count{};
        std::size_t ownerIndex{};
        while (ownerIndex < storage.readbackOwners.size()) {
            auto &slot = storage.slots[storage.readbackOwners[ownerIndex]];
            auto &record = *slot.record;
            for (std::size_t index = 0; index < record.requests.size(); ++index) {
                const auto processed = ProcessVfxGpuObservation(record, index);
                if (processed.HasError())
                    return Outcome::Failure(processed.ErrorValue());
                count += static_cast<std::size_t>(processed.Value());
            }
            if (!FinalizeReadbackOwner(storage, slot))
                ++ownerIndex;
        }
        return Outcome::Success(count);
    }

    /** @copydoc RenderFrontend::FinishVfxGpuStorageAfterNativeDrain */
    void RenderFrontend::FinishVfxGpuStorageAfterNativeDrain() noexcept {
        if (!vfxGpuStorage_)
            return;
        // Backend Drained is the native-domain terminal proof for even uncertain pending observations.
        if (vfxGpuStorage_->readbacks)
            vfxGpuStorage_->readbacks->Shutdown();
        for (auto &slot : vfxGpuStorage_->slots)
            slot.record.reset();
        vfxGpuStorage_->readbackOwners.clear();
        vfxGpuStorage_->reservedReadbackSlots = 0;
    }

}  // namespace Horo::Render
