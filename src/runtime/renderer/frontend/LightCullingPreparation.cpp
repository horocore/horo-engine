#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"

namespace Horo::Render {
    /** @copydoc RenderFrontend::UpdateLightFrame */
    Result<void> RenderFrontend::UpdateLightFrame(const LightFrameBuffers &buffers, const LightFrameUpdate &update) {
        if (activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput, "Update light slots before opening a frame."));
        if (!backend_->Capabilities().support.features.Supports(RenderCapability::LightCulling))
            return Result<void>::Failure(MakeError(LightCullingErrors::Unsupported));
        if (const auto valid = ValidateLightFrameUpdate(update); valid.HasError())
            return valid;
        NativeLightFrameUpdate native{.update = update};
        const std::array handles{buffers.lights, buffers.clusters, buffers.membership, buffers.references};
        for (std::size_t index = 0; index < handles.size(); ++index) {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*this, handles[index]);
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            native.instances[index] = instance.Value();
        }
        try {
            return backend_->UpdateLightFrame(native);
        } catch (...) {  // NOSONAR(cpp:S2738) Translate arbitrary injected/native adapter exceptions at the frontend boundary.
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native light frame update threw."));
        }
    }

    /** @copydoc RenderFrontend::RealizeLightCullingKernel */
    Result<std::shared_ptr<IResidentLightCullingKernel>> RenderFrontend::RealizeLightCullingKernel(const CookedLightCullingKernel &kernel) {
        using Preparation = Result<std::shared_ptr<IResidentLightCullingKernel>>;
        if (activeFrameScope_ != nullptr)
            return Preparation::Failure(MakeError(LightCullingErrors::InvalidInput, "Realize light kernels before opening a frame."));
        if (!backend_->Capabilities().support.features.Supports(RenderCapability::LightCulling))
            return Preparation::Failure(MakeError(LightCullingErrors::Unsupported));
        try {
            return backend_->RealizeLightCullingKernel(kernel);
        } catch (...) {  // NOSONAR(cpp:S2738) Translate arbitrary injected/native adapter exceptions at the frontend boundary.
            return Preparation::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native light kernel preparation threw."));
        }
    }
}  // namespace Horo::Render
