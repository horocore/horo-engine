#pragma once
/** @file VfxGpuStorage.h @brief Single frontend-owned cold GPU simulation inventory and bounded step journal. */
#include "Horo/Runtime/Render/RenderReadback.h"
#include "Horo/Runtime/Render/VfxGpuGraphScheduling.h"

#include <chrono>
#include <memory>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Metadata for one registered immutable candidate; native acceptance is a distinct state. */
    struct VfxGpuFrontendStep final {
        VfxGpuStepIdentity identity;
        CancellationToken unitLifetime;
        CancellationToken callerCancellation;
        std::array<std::uint32_t, VfxGpuDispatchWords::Count> parameters{};
        RenderTimelinePoint submission;
        std::size_t destination{};
        std::size_t observation{};
        bool occupied{};
        bool accepted{};
        bool completed{};
        bool observe{};
        bool dispatched{};
        bool uncertain{};
    };

    /** @brief One existing-queue request, retained until exact native completion and the queue's own consumer retirement fact. */
    struct VfxGpuObservationRequest final {
        VfxGpuStepIdentity step;
        RenderReadbackId id;
        RenderTimelinePoint submission;
        std::chrono::steady_clock::time_point deadline;
        std::size_t observation{};
        bool occupied{};
        bool submitted{};
        bool nativeCompleted{};
        bool nativeUncertain{};
        bool acquired{};
        bool ready{};
        bool cancelRequested{};
        bool timedOut{};
    };

    /** @brief Cold inventory retained by the existing frontend, never by a live VFX world pointer. */
    struct VfxGpuRecord final {
        VfxGpuSimulationId id;
        VfxGpuSimulationDescriptor descriptor;
        std::uint64_t deviceIncarnation{};
        std::size_t nativeBufferBytes{};
        std::size_t reservedCpuPayloadBytes{};
        std::size_t parameterArenaBytes{};
        Sha256Digest simulationProgram;
        Sha256Digest drawProgram;
        CancellationToken lifetime;
        CancellationToken unitLifetime;
        bool unitLifetimeBound{};
        VfxGpuReadbackAdmission readback{VfxGpuReadbackAdmission::Disabled};
        std::shared_ptr<IResidentVfxGpuKernel> kernel;
        std::shared_ptr<IResidentVfxGpuSimulation> native;
        std::vector<ResourceCreation<RenderBufferHandle>> buffers;
        std::vector<NativeVfxGpuBuffer> states;
        std::vector<NativeVfxGpuBuffer> observations;
        NativeVfxGpuBuffer forces;
        NativeVfxGpuBuffer parameterArena;
        NativeVfxGpuPreparationResources preparationResources;
        std::vector<std::uint64_t> generations;
        std::vector<std::uint8_t> observationHeld;
        std::vector<std::uint8_t> observationCompleted;
        std::vector<VfxGpuStepIdentity> observationSteps;
        std::vector<VfxGpuFrontendStep> steps;
        std::vector<VfxGpuObservationRequest> requests;
        std::uint32_t reservedReadbackSlots{};
        std::uint64_t generation{1};
        std::uint64_t nextTick{};
        std::size_t currentSource{};
        std::size_t nextDestination{1};
        RenderTimelinePoint acceptedPoint;
        RenderGraphOwnerId scheduledGraph;
        RenderGraphPassRef computePass;
        std::array<RenderGraphResourceId, 4> graphBuffers;
        std::uint64_t scheduledFrame{};
        bool released{};
        bool stopped{};
    };

    /** @brief Either immutable owner gate can revoke a unit; a caller-mutated payload cannot replace captured ownership. */
    inline bool VfxGpuLifetimeCancelled(const VfxGpuRecord &record) noexcept {
        return record.lifetime.IsCancellationRequested() || (record.unitLifetimeBound && record.unitLifetime.IsCancellationRequested());
    }

    /** @brief Reports actual per-simulation object/vector-capacity payload, without allocator or shared global pool charges. */
    [[nodiscard]] std::size_t VfxGpuRecordPayloadBytes(const VfxGpuRecord &record) noexcept;

    struct VfxGpuStorageSlot final {
        std::unique_ptr<VfxGpuRecord> record;
        std::uint32_t generation{1};
    };

    /** @brief Cold-growing inventory bounded by actual registry buffer admission; no frame path grows its arrays. */
    struct VfxGpuStorage final {
        std::vector<VfxGpuStorageSlot> slots;
        bool busy{};
        std::optional<Error> pendingError;
        std::size_t drainCursor{};
        std::size_t retirementCursor{};
        std::unique_ptr<RenderReadbackQueue> readbacks;
        RenderReadbackPreparedStorage readbackPool;
        RenderReadbackLimits readbackLimits;
        std::chrono::nanoseconds observationTimeout{};
        std::vector<std::uint32_t> readbackOwners;
        std::size_t readbackCpuBytes{};
        std::size_t readbackGlobalRemainderBytes{};
        std::uint32_t reservedReadbackSlots{};
        std::uint64_t observationCopyCalls{};
        std::uint64_t copiedObservationBytes{};
        std::uint64_t observationDiscardCalls{};
        /** @brief Looks up an exact issued ID without allocating or creating a generation. */
        [[nodiscard]] VfxGpuRecord *Find(VfxGpuSimulationId id) noexcept;
        /** @brief Cold-only slot reservation; exhausted generation slots are never reused. */
        [[nodiscard]] Result<std::size_t> ReserveSlot();
    };

    /** @brief Reject-before-construction guards prevent callback reentry from replacing the inventory being inspected. */
    struct VfxGpuBusyGuard final {
        explicit VfxGpuBusyGuard(bool &busy) noexcept : busy_(busy) {
            busy_ = true;
        }

        ~VfxGpuBusyGuard() {
            busy_ = false;
        }

        VfxGpuBusyGuard(const VfxGpuBusyGuard &) = delete;
        VfxGpuBusyGuard &operator=(const VfxGpuBusyGuard &) = delete;

    private:
        bool &busy_;
    };
}  // namespace Horo::Render::Detail
