#include "VulkanGraphRuntimeInternal.h"

namespace Horo::Render::Detail {
    Result<void> VulkanGraphRuntime::Impl::GpuAccess(bool cold) const {
        if (const auto access = Access(cold); access.HasError())
            return access;
        if (cold && lifecycle.frame.IsValid())
            return Result<void>::Failure(MakeError(VulkanBackendErrors::InvalidCommandState));
        return Result<void>::Success();
    }

    bool VulkanGraphRuntime::SupportsVfxGpu() const noexcept {
        return impl_->vfxGpu.Available();
    }

    Result<NativeVfxGpuPreparationResources> VulkanGraphRuntime::QueryVfxGpuNativePreparationResources(
        const VfxGpuSimulationDescriptor &descriptor) const {
        if (const auto access = impl_->GpuAccess(true); access.HasError())
            return Result<NativeVfxGpuPreparationResources>::Failure(access.ErrorValue());
        return impl_->vfxGpu.QueryResources(descriptor);
    }

    Result<std::shared_ptr<IResidentVfxGpuKernel>> VulkanGraphRuntime::RealizeVfxGpuPrograms(const CookedVfxGpuPrograms &programs) {
        if (const auto access = impl_->GpuAccess(true); access.HasError())
            return Result<std::shared_ptr<IResidentVfxGpuKernel>>::Failure(access.ErrorValue());
        return impl_->vfxGpu.Programs(programs);
    }

    Result<std::shared_ptr<IResidentVfxGpuSimulation>> VulkanGraphRuntime::PrepareNativeVfxGpuSimulation(
        const NativeVfxGpuSimulationPreparation &preparation) {
        if (const auto access = impl_->GpuAccess(true); access.HasError())
            return Result<std::shared_ptr<IResidentVfxGpuSimulation>>::Failure(access.ErrorValue());
        return impl_->vfxGpu.Prepare(preparation);
    }

    Result<std::size_t> VulkanGraphRuntime::PollNativeVfxGpuReceipts(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                     std::span<NativeGpuVfxStepReceipt> destination) {
        if (const auto access = impl_->GpuAccess(false); access.HasError())
            return Result<std::size_t>::Failure(access.ErrorValue());
        return impl_->vfxGpu.Poll(simulation, destination);
    }

    Result<std::size_t> VulkanGraphRuntime::CopyCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                           const VfxGpuStepIdentity &step,
                                                                           std::span<std::byte> destination) {
        if (const auto access = impl_->GpuAccess(false); access.HasError())
            return Result<std::size_t>::Failure(access.ErrorValue());
        return impl_->vfxGpu.Copy(simulation, step, destination);
    }

    Result<void> VulkanGraphRuntime::DiscardCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &simulation,
                                                                       const VfxGpuStepIdentity &step) {
        if (const auto access = impl_->GpuAccess(false); access.HasError())
            return Result<void>::Failure(access.ErrorValue());
        return impl_->vfxGpu.Discard(simulation, step);
    }
}  // namespace Horo::Render::Detail
