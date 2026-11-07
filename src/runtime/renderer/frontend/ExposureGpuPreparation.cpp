#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"

namespace Horo::Render {
    /** @copydoc RenderFrontend::RealizeExposureGpuKernel */
    Result<std::shared_ptr<IResidentExposureGpuKernel>> RenderFrontend::RealizeExposureGpuKernel(const CookedExposureGpuKernel &kernel) {
        using Return = Result<std::shared_ptr<IResidentExposureGpuKernel>>;
        if (auto owner = RequireOpenOwner(); owner.HasError())
            return Return::Failure(owner.ErrorValue());
        if (activeFrameScope_ != nullptr)
            return Return::Failure(MakeError(ExposureGpuErrors::InvalidInput));
        if (!backend_->Capabilities().support.features.Supports(RenderCapability::ExposureGpu))
            return Return::Failure(MakeError(ExposureGpuErrors::Unsupported));
        try {
            return backend_->RealizeExposureGpuKernel(kernel);
        } catch (...) {  // NOSONAR(cpp:S2738) Translate injected/native exceptions at the frontend ownership boundary.
            return Return::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native exposure preparation threw."));
        }
    }
}  // namespace Horo::Render
