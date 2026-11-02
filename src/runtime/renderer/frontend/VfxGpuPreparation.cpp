#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"
#include "VfxFrontendState.h"
#include "VfxGpuInventoryLease.h"
#include "VfxGpuReadback.h"
#include "VfxGpuStorage.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Render {
    namespace {
        using namespace Detail;

        bool SamePreparation(const VfxGpuRecord &record, const VfxGpuSimulationPreparation &preparation,
                             const CookedVfxGpuPrograms &programs) noexcept {
            return !record.released && SameVfxGpuIdentity(record.descriptor.identity, preparation.descriptor.identity) &&
                   record.descriptor.initializationDigest == preparation.descriptor.initializationDigest &&
                   record.simulationProgram == programs.simulation.artifact.artifactKey &&
                   record.drawProgram == programs.draw.artifact.artifactKey;
        }

        Result<VfxGpuRecord *> FindExisting(VfxGpuStorage &storage, const VfxGpuSimulationPreparation &preparation,
                                            const CookedVfxGpuPrograms &programs) {
            for (const auto &slot : storage.slots) {
                if (!slot.record || !SameVfxGpuIdentity(slot.record->descriptor.identity, preparation.descriptor.identity))
                    continue;
                if (SamePreparation(*slot.record, preparation, programs))
                    return Result<VfxGpuRecord *>::Success(slot.record.get());
                return Result<VfxGpuRecord *>::Failure(
                    MakeError(VfxGpuSimulationErrors::InvalidPreparation,
                              "Release the previous exact logical simulation before replacing its source or cooked family."));
            }
            return Result<VfxGpuRecord *>::Success(nullptr);
        }

        Result<void> SupportsGpuPreparation(const RenderBackendCapabilities &caps, const VfxGpuReadbackDescriptor &readback) {
            const auto &features = caps.support.features;
            if (!features.Supports(RenderCapability::VfxGpuSimulation) || !features.Supports(RenderCapability::VfxGpuDraw) ||
                !caps.supportsExplicitGraphSubmissions || !caps.supportsSubmissionObservation)
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceUnsupported));
            if (readback.enabled && readback.requirement == VfxGpuReadbackRequirement::RequiredForVisualEffect &&
                !features.Supports(RenderCapability::VfxGpuReadback))
                return Result<void>::Failure(MakeError(FrontendErrors::ResourceUnsupported));
            return Result<void>::Success();
        }

        /** @brief Initial queue ownership is explicit: only exact pending retries may drain an already occupied queue. */
        Result<void> QueueIsIdle(const RenderFrontend &frontend) {
            if (frontend.UploadSnapshot().pendingRequests != 0)
                return Result<void>::Failure(
                    MakeError(VfxFrameErrors::Pending, "Finish existing resource uploads before GPU preparation."));
            return Result<void>::Success();
        }

        /** @brief Rolls back every already queued prefix handle if a cold allocation/callback fails. */
        struct Rollback final {
            RenderFrontend &frontend;
            VfxGpuRecord &record;
            VfxGpuStorage &storage;
            bool keep{};

            ~Rollback() {
                if (keep)
                    return;
                record.native.reset();
                ReleaseVfxGpuReadbackReservation(storage, record);
                for (const auto &buffer : record.buffers)
                    static_cast<void>(frontend.ReleaseBuffer(buffer.handle));
            }
        };

        Result<void> ResolveInventory(RenderResourceRegistry &registry, VfxGpuRecord &record) {
            for (auto &buffer : record.states) {
                const auto native = registry.BackendInstance(RenderResourceClass::Buffer,
                                                             {buffer.handle.owner, buffer.handle.slot, buffer.handle.generation});
                if (native.HasError())
                    return Result<void>::Failure(native.ErrorValue());
                buffer.instance = native.Value();
            }
            for (auto &buffer : record.observations) {
                const auto native = registry.BackendInstance(RenderResourceClass::Buffer,
                                                             {buffer.handle.owner, buffer.handle.slot, buffer.handle.generation});
                if (native.HasError())
                    return Result<void>::Failure(native.ErrorValue());
                buffer.instance = native.Value();
            }
            const auto native =
                registry.BackendInstance(RenderResourceClass::Buffer,
                                         {record.forces.handle.owner, record.forces.handle.slot, record.forces.handle.generation});
            if (native.HasError())
                return Result<void>::Failure(native.ErrorValue());
            record.forces.instance = native.Value();
            return Result<void>::Success();
        }

        Result<bool> AllReady(const RenderFrontend &frontend, const VfxGpuRecord &record) {
            for (const auto &buffer : record.buffers) {
                const auto state = frontend.ResourceState(buffer.handle);
                if (state.HasError())
                    return Result<bool>::Failure(state.ErrorValue());
                if (state.Value() == RenderResourceState::Pending)
                    return Result<bool>::Success(false);
                if (state.Value() == RenderResourceState::Failed) {
                    const auto failed = frontend.ResourceOperationResult(buffer.operation);
                    if (failed.HasError())
                        return Result<bool>::Failure(failed.ErrorValue());
                }
                if (state.Value() != RenderResourceState::Ready)
                    return Result<bool>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation,
                                                           "A prepared GPU particle buffer failed or retired before native admission."));
            }
            return Result<bool>::Success(true);
        }

        /** @brief Native callbacks cannot publish a receipt after source retirement, lifetime revocation or device replacement. */
        Result<void> PublicationReady(const RenderFrontend &frontend, const VfxGpuRecord &record, const std::uint64_t device) {
            if (VfxGpuLifetimeCancelled(record))
                return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
            if (record.deviceIncarnation != device)
                return Result<void>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
            const auto ready = AllReady(frontend, record);
            if (ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
            return ready.Value() ? Result<void>::Success() : Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        }
    }  // namespace

    /** @copydoc RenderFrontend::AdmitVfxGpuPreparation */
    Result<void> RenderFrontend::AdmitVfxGpuPreparation(const VfxGpuSimulationPreparation &preparation, const CancellationToken &lifetime) {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        if (vfxGpuStorage_ && vfxGpuStorage_->busy)
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending));
        DrainVfxGpuRetirements();
        if (lifetime.IsCancellationRequested())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
        if (const auto support = SupportsGpuPreparation(backend_->Capabilities(), preparation.descriptor.readback); support.HasError())
            return support;
        return ValidateVfxGpuSimulationPreparation(preparation);
    }

    /** @copydoc RenderFrontend::PrepareVfxGpuSimulation */
    Result<VfxGpuPreparationReceipt> RenderFrontend::PrepareVfxGpuSimulation(const VfxGpuSimulationPreparation &preparation,
                                                                             const CookedVfxGpuPrograms &programs,
                                                                             const CancellationToken &lifetime) {
        using Outcome = Result<VfxGpuPreparationReceipt>;
        if (const auto admitted = AdmitVfxGpuPreparation(preparation, lifetime); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        try {
            if (!vfxGpuStorage_)
                vfxGpuStorage_ = std::make_unique<Detail::VfxGpuStorage>();
            if (!vfxFrontendState_)
                vfxFrontendState_ = std::make_shared<const Detail::VfxFrontendState>();
            if (vfxGpuStorage_->busy)
                return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
            Detail::VfxGpuBusyGuard guard{vfxGpuStorage_->busy};
            const auto record = FindOrCreateVfxGpuRecord(preparation, programs, lifetime);
            if (record.HasError())
                return Outcome::Failure(record.ErrorValue());
            return ResumeVfxGpuPreparation(*record.Value(), preparation);
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        } catch (const std::length_error &) {
            return Outcome::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        } catch (...) {  // Native callbacks cannot unwind across the frontend after transactional rollback.
            return Outcome::Failure(MakeError(FrontendErrors::ResourceBackendException));
        }
    }

    /** @copydoc RenderFrontend::FindOrCreateVfxGpuRecord */
    Result<Detail::VfxGpuRecord *> RenderFrontend::FindOrCreateVfxGpuRecord(const VfxGpuSimulationPreparation &preparation,
                                                                            const CookedVfxGpuPrograms &programs,
                                                                            const CancellationToken &lifetime) {
        using Outcome = Result<Detail::VfxGpuRecord *>;
        const auto existing = FindExisting(*vfxGpuStorage_, preparation, programs);
        if (existing.HasError() || existing.Value())
            return existing;
        if (const auto idle = QueueIsIdle(*this); idle.HasError())
            return Outcome::Failure(idle.ErrorValue());
        const auto reserved = vfxGpuStorage_->ReserveSlot();
        if (reserved.HasError())
            return Outcome::Failure(reserved.ErrorValue());
        auto record = std::make_unique<Detail::VfxGpuRecord>();
        Rollback rollback{*this, *record, *vfxGpuStorage_};
        auto &slot = vfxGpuStorage_->slots[reserved.Value()];
        record->id = {resourceRegistry_->Owner(), static_cast<std::uint32_t>(reserved.Value()), slot.generation};
        record->descriptor = preparation.descriptor;
        record->deviceIncarnation = backend_->Capabilities().support.deviceIncarnation;
        record->simulationProgram = programs.simulation.artifact.artifactKey;
        record->drawProgram = programs.draw.artifact.artifactKey;
        record->lifetime = lifetime;
        const auto kernel = backend_->RealizeVfxGpuPrograms(programs);
        if (kernel.HasError())
            return Outcome::Failure(kernel.ErrorValue());
        record->kernel = kernel.Value();
        // Native cold maintenance releases pins; the canonical bounded drain retires their actual backing before reuse.
        if (const auto processed = ProcessResourceRequests(); processed.HasError())
            return Outcome::Failure(processed.ErrorValue());
        if (const auto reservedReadback =
                Detail::ReserveVfxGpuReadback(*vfxGpuStorage_, *record,
                                              backend_->Capabilities().support.features.Supports(RenderCapability::VfxGpuReadback));
            reservedReadback.HasError())
            return Outcome::Failure(reservedReadback.ErrorValue());
        if (const auto created = CreateVfxGpuBuffers(*record, preparation); created.HasError())
            return Outcome::Failure(created.ErrorValue());
        rollback.keep = true;
        slot.record = std::move(record);
        return Outcome::Success(slot.record.get());
    }

    /** @copydoc RenderFrontend::ResumeVfxGpuPreparation */
    Result<VfxGpuPreparationReceipt> RenderFrontend::ResumeVfxGpuPreparation(Detail::VfxGpuRecord &record,
                                                                             const VfxGpuSimulationPreparation &preparation) {
        const auto rollback = [this](Detail::VfxGpuRecord *candidate) noexcept {
            RollbackVfxGpuRecord(candidate->id);
        };
        std::unique_ptr<Detail::VfxGpuRecord, decltype(rollback)> transaction{&record, rollback};
        auto result = CompleteVfxGpuPreparation(record, preparation);
        if (result.HasValue() || ErrorChainContains(result.ErrorValue(), VfxFrameErrors::Pending.domain, VfxFrameErrors::Pending.code))
            static_cast<void>(transaction.release());
        return result;
    }

    /** @copydoc RenderFrontend::CompleteVfxGpuPreparation */
    Result<VfxGpuPreparationReceipt> RenderFrontend::CompleteVfxGpuPreparation(Detail::VfxGpuRecord &record,
                                                                               const VfxGpuSimulationPreparation &preparation) {
        using Outcome = Result<VfxGpuPreparationReceipt>;
        if (Detail::VfxGpuLifetimeCancelled(record) || record.released)
            return Outcome::Failure(MakeError(VfxFrameErrors::Cancelled));
        if (record.deviceIncarnation != backend_->Capabilities().support.deviceIncarnation)
            return Outcome::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        if (const auto processed = ProcessResourceRequests(); processed.HasError())
            return Outcome::Failure(processed.ErrorValue());
        const auto ready = AllReady(*this, record);
        if (ready.HasError())
            return Outcome::Failure(ready.ErrorValue());
        if (!ready.Value())
            return Outcome::Failure(MakeError(VfxFrameErrors::Pending));
        if (record.native)
            return Outcome::Success(IssueVfxGpuReceipt(record));
        if (const auto resolved = ResolveInventory(*resourceRegistry_, record); resolved.HasError())
            return Outcome::Failure(resolved.ErrorValue());
        if (const auto realized = RealizeVfxGpuInventory(record, preparation); realized.HasError())
            return Outcome::Failure(realized.ErrorValue());
        return Outcome::Success(IssueVfxGpuReceipt(record));
    }

    /** @copydoc RenderFrontend::RealizeVfxGpuInventory */
    Result<void> RenderFrontend::RealizeVfxGpuInventory(Detail::VfxGpuRecord &record, const VfxGpuSimulationPreparation &preparation) {
        std::vector<RenderBufferHandle> handles;
        handles.reserve(record.buffers.size());
        for (const auto &buffer : record.buffers)
            handles.push_back(buffer.handle);
        const auto lease = Detail::VfxGpuInventoryLease::Create(*resourceRegistry_, handles, vfxFrontendState_);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        const auto native = backend_->PrepareNativeVfxGpuSimulation({record.id, record.deviceIncarnation, preparation, record.kernel,
                                                                     record.states, record.forces, record.observations, lease.Value()});
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        if (!native.Value())
            return Result<void>::Failure(MakeError(VfxGpuSimulationErrors::InvalidPreparation));
        std::size_t remaining = record.descriptor.maximumRetainedBytes;
        const std::array costs{record.nativeBufferBytes, Detail::VfxGpuRecordPayloadBytes(record), lease.Value()->RetainedPayloadBytes(),
                               native.Value()->RetainedPayloadBytes(), record.reservedCpuPayloadBytes};
        for (const auto bytes : costs) {
            if (bytes > remaining)
                return Result<void>::Failure(
                    MakeError(FrontendErrors::ResourceCapacityExhausted,
                              "Actual GPU simulation buffers, metadata and reserved CPU payload exceed the admitted ledger slice."));
            remaining -= bytes;
        }
        record.native = native.Value();
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        return PublicationReady(*this, record, backend_->Capabilities().support.deviceIncarnation);
    }

    /** @copydoc RenderFrontend::IssueVfxGpuReceipt */
    VfxGpuPreparationReceipt RenderFrontend::IssueVfxGpuReceipt(const Detail::VfxGpuRecord &record) const {
        VfxGpuPreparationReceipt receipt;
        receipt.descriptor_ = record.descriptor;
        receipt.authority_ = vfxFrontendState_;
        receipt.id_ = record.id;
        receipt.deviceIncarnation_ = record.deviceIncarnation;
        receipt.readback_ = record.readback;
        return receipt;
    }
}  // namespace Horo::Render
