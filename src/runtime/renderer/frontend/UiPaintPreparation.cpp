#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"

namespace Horo::Render {
    /** @copydoc RenderFrontend::UpdateUiPaintFrame */
    Result<void> RenderFrontend::UpdateUiPaintFrame(const UiPaintFrameBuffers &buffers, const UiPaintFrameUpdate &update) {
        return UpdateUiPaintFrameInternal({}, buffers, update);
    }

    /** @copydoc RenderFrontend::UpdateUiPaintFrameInternal */
    Result<void> RenderFrontend::UpdateUiPaintFrameInternal(const FrameToken frame, const UiPaintFrameBuffers &buffers,
                                                            const UiPaintFrameUpdate &update) {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (frame.IsValid() != (activeFrameScope_ != nullptr) || (activeFrameScope_ && activeFrameScope_->Token() != frame))
            return Result<void>::Failure(
                MakeError(RenderGraphExecutionErrors::InvalidGraph, "UI slot upload requires its exact pre-record scene frame."));
        if (activeFrameScope_)
            if (const auto admitted = activeFrameScope_->ValidateExecutionAdmission(); admitted.HasError())
                return admitted;
        if (const auto valid = ValidateUiPaintFrameUpdate(update); valid.HasError())
            return valid;
        const std::array handles{buffers.vertices, buffers.indices, buffers.parameters};
        NativeUiPaintFrameUpdate native{.update = update, .frame = frame};
        for (std::size_t index = 0; index < handles.size(); ++index) {
            for (std::size_t previous = 0; previous < index; ++previous)
                if (handles[index] == handles[previous])
                    return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*this, handles[index]);
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            native.instances[index] = instance.Value();
        }
        try {
            // The backend owns the final admission and revision publication. Moving the frontend scope preserves its token.
            return backend_->UpdateUiPaintFrame(native);
        } catch (...) {  // NOSONAR(cpp:S2738) Frontend contains exceptions from the native backend boundary.
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native UI frame update threw."));
        }
    }

    /** @copydoc RenderFrontend::RealizeUiPaintKernel */
    Result<std::shared_ptr<IResidentUiPaintKernel>> RenderFrontend::RealizeUiPaintKernel(const CookedUiPaintKernel &kernel) {
        using Preparation = Result<std::shared_ptr<IResidentUiPaintKernel>>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Preparation::Failure(owner.ErrorValue());
        if (activeFrameScope_)
            return Preparation::Failure(
                MakeError(RenderGraphExecutionErrors::InvalidGraph, "Prepare UI pipelines before opening a frame."));
        try {
            return backend_->RealizeUiPaintKernel(kernel);
        } catch (...) {  // NOSONAR(cpp:S2738) Frontend contains exceptions from the native backend boundary.
            return Preparation::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native UI pipeline preparation threw."));
        }
    }
}  // namespace Horo::Render
