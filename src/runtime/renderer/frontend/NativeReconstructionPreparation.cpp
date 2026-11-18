#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"

namespace Horo::Render {
    /** @copydoc RenderFrontend::RealizeNativeReconstructionKernel */
    Result<std::shared_ptr<IResidentNativeReconstructionKernel>> RenderFrontend::RealizeNativeReconstructionKernel(
        const CookedNativeReconstructionKernel &kernel) {
        using Outcome = Result<std::shared_ptr<IResidentNativeReconstructionKernel>>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (activeFrameScope_ != nullptr)
            return Outcome::Failure(MakeError(NativeReconstructionErrors::InvalidDispatch));
        if (!backend_->Capabilities().support.features.Supports(RenderCapability::NativeReconstruction))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        try {
            return backend_->RealizeNativeReconstructionKernel(kernel);
        } catch (...) {  // NOSONAR(cpp:S2738) Preserve the frontend exception boundary for native implementations.
            return Outcome::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native reconstruction preparation threw."));
        }
    }
}  // namespace Horo::Render
