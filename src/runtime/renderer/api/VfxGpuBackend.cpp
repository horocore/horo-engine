#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

namespace Horo::Render {
    /** @copydoc IRenderBackend::RealizeVfxGpuPrograms */
    Result<std::shared_ptr<IResidentVfxGpuKernel>> IRenderBackend::RealizeVfxGpuPrograms(const CookedVfxGpuPrograms &) {
        return Result<std::shared_ptr<IResidentVfxGpuKernel>>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }

    /** @copydoc IRenderBackend::PrepareNativeVfxGpuSimulation */
    Result<std::shared_ptr<IResidentVfxGpuSimulation>> IRenderBackend::PrepareNativeVfxGpuSimulation(
        const NativeVfxGpuSimulationPreparation &) {
        return Result<std::shared_ptr<IResidentVfxGpuSimulation>>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }

    /** @copydoc IRenderBackend::PollNativeVfxGpuReceipts */
    Result<std::size_t> IRenderBackend::PollNativeVfxGpuReceipts(const std::shared_ptr<IResidentVfxGpuSimulation> &,
                                                                 std::span<NativeGpuVfxStepReceipt>) {
        return Result<std::size_t>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }

    /** @copydoc IRenderBackend::CopyCompletedVfxGpuObservation */
    Result<std::size_t> IRenderBackend::CopyCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &,
                                                                       const VfxGpuStepIdentity &, std::span<std::byte>) {
        return Result<std::size_t>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }

    /** @copydoc IRenderBackend::DiscardCompletedVfxGpuObservation */
    Result<void> IRenderBackend::DiscardCompletedVfxGpuObservation(const std::shared_ptr<IResidentVfxGpuSimulation> &,
                                                                   const VfxGpuStepIdentity &) {
        return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::UnsupportedWorkload));
    }
}  // namespace Horo::Render
