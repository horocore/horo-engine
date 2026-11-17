#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "MetalResourceRuntimeInternal.h"
#include "MetalVfxGpuState.h"

#include <algorithm>
#include <cstring>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Exact held step is authoritative; generations alone cannot identify an observation request. */
        Result<MetalVfxGpuBuffer *> CompletedObservation(MetalVfxGpuSimulation &simulation, const VfxGpuStepIdentity &step) {
            simulation.ObserveCompletions();
            const auto found = std::ranges::find_if(simulation.observations, [&](const auto &output) {
                return output.observationHeld && output.observationStep == step;
            });
            if (found == simulation.observations.end())
                return Result<MetalVfxGpuBuffer *>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            if (!found->observationCompleted)
                return Result<MetalVfxGpuBuffer *>::Failure(MakeError(VfxFrameErrors::Pending));
            return Result<MetalVfxGpuBuffer *>::Success(&*found);
        }

        /** @brief Failed native work can be discarded but can never publish a successful readback. */
        Result<std::size_t> CopyObservation(MetalVfxGpuSimulation &simulation, MetalVfxGpuBuffer &output,
                                            const std::span<std::byte> destination) {
            const auto bytes = std::size_t{simulation.descriptor.readback.maximumRecords} * VfxGpuObservationWords::Bytes;
            if (output.observationFailed)
                return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
            if (destination.size() < bytes || output.native->buffer.storageMode != MTLStorageModeShared ||
                output.native->buffer.contents == nullptr)
                return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            std::memmove(destination.data(), output.native->buffer.contents, bytes);
            output.observationHeld = false;
            return Result<std::size_t>::Success(bytes);
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::CopyCompletedVfxGpuObservation */
    Result<std::size_t> MetalResourceRuntime::CopyCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &source,
                                                                             const VfxGpuStepIdentity &step,
                                                                             const std::span<std::byte> destination) {
        const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(source);
        if (!simulation || !simulation->resources || simulation->owner != impl_->vfxGpuOwner ||
            simulation->incarnation != impl_->lightKernelIncarnation)
            return Result<std::size_t>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto output = CompletedObservation(*simulation, step);
        if (output.HasError())
            return Result<std::size_t>::Failure(output.ErrorValue());
        return CopyObservation(*simulation, *output.Value(), destination);
    }

    /** @copydoc MetalResourceRuntime::DiscardCompletedVfxGpuObservation */
    Result<void> MetalResourceRuntime::DiscardCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &source,
                                                                         const VfxGpuStepIdentity &step) {
        const auto simulation = std::dynamic_pointer_cast<MetalVfxGpuSimulation>(source);
        if (!simulation || !simulation->resources || simulation->owner != impl_->vfxGpuOwner ||
            simulation->incarnation != impl_->lightKernelIncarnation)
            return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
        const auto output = CompletedObservation(*simulation, step);
        if (output.HasError())
            return Result<void>::Failure(output.ErrorValue());
        output.Value()->observationHeld = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
