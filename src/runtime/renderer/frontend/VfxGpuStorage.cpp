#include "VfxGpuStorage.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "VfxGpuReadback.h"

#include <algorithm>
#include <limits>

namespace Horo::Render::Detail {
    /** @copydoc VfxGpuRecordPayloadBytes */
    std::size_t VfxGpuRecordPayloadBytes(const VfxGpuRecord &record) noexcept {
        return sizeof(record) + record.buffers.capacity() * sizeof(ResourceCreation<RenderBufferHandle>) +
               (record.states.capacity() + record.observations.capacity()) * sizeof(NativeVfxGpuBuffer) +
               record.generations.capacity() * sizeof(std::uint64_t) + record.steps.capacity() * sizeof(VfxGpuFrontendStep) +
               record.observationHeld.capacity() + record.observationCompleted.capacity() +
               record.observationSteps.capacity() * sizeof(VfxGpuStepIdentity) +
               record.requests.capacity() * sizeof(VfxGpuObservationRequest);
    }

    /** @copydoc VfxGpuStorage::Find */
    VfxGpuRecord *VfxGpuStorage::Find(const VfxGpuSimulationId id) noexcept {
        if (!id.IsValid() || id.slot >= slots.size())
            return nullptr;
        auto *record = slots[id.slot].record.get();
        return record && record->id == id ? record : nullptr;
    }

    /** @copydoc VfxGpuStorage::ReserveSlot */
    Result<std::size_t> VfxGpuStorage::ReserveSlot() {
        for (std::size_t index = 0; index < slots.size(); ++index)
            if (!slots[index].record && slots[index].generation != 0)
                return Result<std::size_t>::Success(index);
        if (slots.size() == std::numeric_limits<std::uint32_t>::max())
            return Result<std::size_t>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        slots.emplace_back();
        return Result<std::size_t>::Success(slots.size() - 1);
    }
}  // namespace Horo::Render::Detail

namespace Horo::Render {
    namespace {
        /** @brief Cold inventory builder; all native storage is created by ordinary frontend APIs and cost plans. */
        struct GpuBufferBuilder final {
            RenderFrontend &frontend;
            IRenderBackend &backend;
            Detail::VfxGpuRecord &record;
            std::size_t charged{};

            Result<void> Charge(const RenderBufferDescriptor &descriptor, const std::size_t count) {
                const auto plan = backend.QueryBufferMemoryCost(descriptor);
                if (plan.HasError())
                    return Result<void>::Failure(plan.ErrorValue());
                if (const auto limit = record.descriptor.maximumRetainedBytes;
                    !plan.Value().IsValid() || charged > limit || count > (limit - charged) / plan.Value().requiredBytes)
                    return Result<void>::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted,
                                                           "Actual GPU particle native cost exceeds its retained-byte ledger slice."));
                charged += count * plan.Value().requiredBytes;
                return Result<void>::Success();
            }

            Result<void> Queue(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> bytes,
                               NativeVfxGpuBuffer &target) {
                const auto created = frontend.CreateBuffer(descriptor, bytes);
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                record.buffers.push_back(created.Value());
                target.handle = created.Value().handle;
                return Result<void>::Success();
            }

            Result<void> States(const VfxGpuSimulationPreparation &source) {
                using enum Horo::Render::RenderBufferUsage;
                const RenderBufferDescriptor seed{source.descriptor.layout.byteCount, Storage | CopySource,
                                                  RenderBufferAccess::HostVisible};
                const RenderBufferDescriptor output{source.descriptor.layout.byteCount, Storage | CopySource | CopyDestination,
                                                    RenderBufferAccess::DeviceLocal};
                if (const auto cost = Charge(seed, 1); cost.HasError())
                    return cost;
                if (const auto cost = Charge(output, record.states.size() - 1); cost.HasError())
                    return cost;
                if (const auto queued = Queue(seed, std::as_bytes(source.initialWords), record.states[0]); queued.HasError())
                    return queued;
                for (std::size_t index = 1; index < record.states.size(); ++index)
                    if (const auto queued = Queue(output, {}, record.states[index]); queued.HasError())
                        return queued;
                return Result<void>::Success();
            }

            Result<void> Forces(const VfxGpuSimulationPreparation &source) {
                const std::array<std::uint32_t, 12> empty{};
                const auto words = source.forceWords.empty() ? std::span<const std::uint32_t>{empty} : source.forceWords;
                const RenderBufferDescriptor descriptor{words.size_bytes(), RenderBufferUsage::Storage, RenderBufferAccess::HostVisible};
                if (const auto cost = Charge(descriptor, 1); cost.HasError())
                    return cost;
                return Queue(descriptor, std::as_bytes(words), record.forces);
            }

            /** @brief Queues the already charged arena through the same ordinary registry as every particle buffer. */
            Result<void> ParameterArena() {
                const auto &descriptor = record.preparationResources.parameterArena;
                return descriptor.byteSize ? Queue(descriptor, {}, record.parameterArena) : Result<void>::Success();
            }

            Result<void> Observations() {
                const auto &readback = record.descriptor.readback;
                const auto bytes = readback.enabled ? std::size_t{readback.maximumRecords} * VfxGpuObservationWords::Bytes : 4;
                const RenderBufferDescriptor descriptor{bytes, RenderBufferUsage::Storage, RenderBufferAccess::HostVisible};
                if (const auto cost = Charge(descriptor, record.observations.size()); cost.HasError())
                    return cost;
                const std::array<std::byte, 4> zero{};
                const auto initial = readback.enabled ? std::span<const std::byte>{} : std::span<const std::byte>{zero};
                for (auto &observation : record.observations)
                    if (const auto queued = Queue(descriptor, initial, observation); queued.HasError())
                        return queued;
                return Result<void>::Success();
            }
        };
    }  // namespace

    /** @copydoc RenderFrontend::CreateVfxGpuBuffers */
    Result<void> RenderFrontend::CreateVfxGpuBuffers(Detail::VfxGpuRecord &record, const VfxGpuSimulationPreparation &preparation) {
        const auto states = VfxGpuStateBufferCount(record.descriptor);
        const auto &readback = record.descriptor.readback;
        const auto observations = readback.enabled ? readback.maximumPendingResults : 1U;
        record.buffers.reserve(std::size_t{states} + observations + 1 + (record.preparationResources.parameterArena.byteSize != 0));
        record.states.resize(states);
        record.observations.resize(observations);
        record.generations.resize(states);
        record.generations[0] = 1;
        record.observationHeld.resize(observations);
        record.observationCompleted.resize(observations);
        record.observationSteps.resize(observations);
        record.steps.resize(record.descriptor.maximumPendingSteps);
        GpuBufferBuilder builder{*this, *backend_, record, record.parameterArenaBytes};
        if (const auto created = builder.States(preparation); created.HasError())
            return created;
        if (const auto created = builder.Forces(preparation); created.HasError())
            return created;
        if (const auto created = builder.Observations(); created.HasError())
            return created;
        if (const auto created = builder.ParameterArena(); created.HasError())
            return created;
        record.nativeBufferBytes = builder.charged;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::RetireVfxGpuRecord */
    void RenderFrontend::RetireVfxGpuRecord(Detail::VfxGpuRecord &record) noexcept {
        record.released = record.stopped = true;
        for (auto &request : record.requests)
            if (request.occupied)
                request.cancelRequested = true;
        if (const bool nativeObservation = std::ranges::any_of(record.observationHeld,
                                                               [](auto held) {
            return held != 0;
        });
            !nativeObservation)
            record.native.reset();
        for (const auto &buffer : record.buffers)
            static_cast<void>(ReleaseBuffer(buffer.handle));
        record.buffers.clear();
        Detail::ReleaseVfxGpuReadbackReservation(*vfxGpuStorage_, record);
    }

    /** @copydoc RenderFrontend::ReleaseVfxGpuSimulation */
    Result<void> RenderFrontend::ReleaseVfxGpuSimulation(const VfxGpuPreparationReceipt &preparation) {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        if (!vfxGpuStorage_ || vfxGpuStorage_->busy || !preparation.IsLive())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        auto *record = vfxGpuStorage_->Find(preparation.Id());
        if (!record)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        RetireVfxGpuRecord(*record);
        if (record->reservedReadbackSlots == 0) {
            auto &slot = vfxGpuStorage_->slots[preparation.Id().slot];
            slot.record.reset();
            ++slot.generation;
        }
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::RollbackVfxGpuRecord */
    void RenderFrontend::RollbackVfxGpuRecord(const VfxGpuSimulationId id) noexcept {
        auto &slot = vfxGpuStorage_->slots[id.slot];
        RetireVfxGpuRecord(*slot.record);
        slot.record.reset();
        ++slot.generation;
    }

    /** @copydoc RenderFrontend::DrainVfxGpuRetirements */
    void RenderFrontend::DrainVfxGpuRetirements() noexcept {
        if (!vfxGpuStorage_ || vfxGpuStorage_->busy)
            return;
        Detail::VfxGpuBusyGuard guard{vfxGpuStorage_->busy};
        const auto visits = std::min<std::size_t>(vfxGpuRetirementBudget_, vfxGpuStorage_->slots.size());
        for (std::size_t index = 0; index < visits; ++index) {
            const auto current = vfxGpuStorage_->retirementCursor++ % vfxGpuStorage_->slots.size();
            auto &slot = vfxGpuStorage_->slots[current];
            if (slot.record && Detail::VfxGpuLifetimeCancelled(*slot.record)) {
                RetireVfxGpuRecord(*slot.record);
                if (slot.record->reservedReadbackSlots == 0) {
                    slot.record.reset();
                    ++slot.generation;
                }
            }
        }
    }

    /** @copydoc RenderFrontend::CloseVfxGpuStorage */
    void RenderFrontend::CloseVfxGpuStorage() noexcept {
        if (!vfxGpuStorage_)
            return;
        if (vfxGpuStorage_->readbacks)
            vfxGpuStorage_->readbacks->StopAdmission();
        for (auto &slot : vfxGpuStorage_->slots)
            if (slot.record)
                slot.record->released = slot.record->stopped = true;
    }
}  // namespace Horo::Render
