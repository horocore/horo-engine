#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderExternalImages.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"

namespace Horo::Render::Detail {
    /** @copydoc RenderExternalTextureAccess::ValidateImport */
    Result<IRenderExternalTextureBackend *> RenderExternalTextureAccess::ValidateImport(
        const RenderFrontend &frontend, const std::shared_ptr<const IRenderExternalTextureSource> &source) {
        if (std::this_thread::get_id() != frontend.preparationOwner_ || frontend.activeFrameScope_ != nullptr || !source)
            return Result<IRenderExternalTextureBackend *>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        if (frontend.closing_ || source->DeviceIncarnation() == 0 ||
            source->DeviceIncarnation() != frontend.backend_->Capabilities().support.deviceIncarnation)
            return Result<IRenderExternalTextureBackend *>::Failure(MakeError(FrontendErrors::ResourceUnsupported));
        if (auto valid = source->Validate(); valid.HasError())
            return Result<IRenderExternalTextureBackend *>::Failure(valid.ErrorValue());
        if (!source->Descriptor().IsValid())
            return Result<IRenderExternalTextureBackend *>::Failure(MakeError(FrontendErrors::InvalidTextureDescriptor));
        auto *backend = dynamic_cast<IRenderExternalTextureBackend *>(frontend.backend_.get());
        if (backend == nullptr)
            return Result<IRenderExternalTextureBackend *>::Failure(MakeError(FrontendErrors::ResourceUnsupported));
        return Result<IRenderExternalTextureBackend *>::Success(backend);
    }

    /** @copydoc RenderExternalTextureAccess::Import */
    Result<RenderTextureHandle> RenderExternalTextureAccess::Import(RenderFrontend &frontend,
                                                                    std::shared_ptr<const IRenderExternalTextureSource> source) {
        using Outcome = Result<RenderTextureHandle>;
        const auto backend = ValidateImport(frontend, source);
        if (backend.HasError())
            return Outcome::Failure(backend.ErrorValue());
        auto reservation = frontend.resourceRegistry_->Reserve(RenderResourceClass::Texture);
        if (reservation.HasError())
            return Outcome::Failure(reservation.ErrorValue());
        const auto identity = reservation.Value().identity;
        std::uint64_t instance{};
        try {
            // Prepare all owning metadata before the native adapter can retain the borrow.
            if (identity.slot >= frontend.textures_.size())
                frontend.textures_.resize(static_cast<std::size_t>(identity.slot) + 1);
            frontend.textures_[identity.slot] = {.generation = identity.generation, .descriptor = source->Descriptor()};
            auto attached = frontend.resourceRegistry_->AttachExternalSource(identity, source);
            if (attached.HasError()) {
                frontend.resourceRegistry_->RollbackPending(RenderResourceClass::Texture, reservation.Value());
                return Outcome::Failure(attached.ErrorValue());
            }
            auto lifetime = frontend.resourceRegistry_->PrepareExternalNativeLifetime(identity);
            if (lifetime.HasError()) {
                frontend.resourceRegistry_->RollbackPending(RenderResourceClass::Texture, reservation.Value());
                return Outcome::Failure(lifetime.ErrorValue());
            }
            auto imported = backend.Value()->ImportExternalTexture(*source, std::move(lifetime.Value()));
            if (imported.HasError()) {
                frontend.resourceRegistry_->RollbackPending(RenderResourceClass::Texture, reservation.Value());
                return Outcome::Failure(imported.ErrorValue());
            }
            instance = imported.Value();
            const auto published = frontend.resourceRegistry_->Publish(RenderResourceClass::Texture, identity, instance);
            if (published.HasError()) {
                frontend.backend_->DestroyTexture(instance);
                instance = 0;
                frontend.resourceRegistry_->RollbackPending(RenderResourceClass::Texture, reservation.Value());
                return Outcome::Failure(published.ErrorValue());
            }
            return Outcome::Success(TextureHandle(identity));
        } catch (...) {
            if (instance != 0)
                frontend.backend_->DestroyTexture(instance);
            frontend.resourceRegistry_->RollbackPending(RenderResourceClass::Texture, reservation.Value());
            throw;
        }
    }

    /** @copydoc RenderExternalTextureAccess::RetirementComplete */
    Result<bool> RenderExternalTextureAccess::RetirementComplete(const RenderFrontend &frontend, const RenderTextureHandle texture) {
        if (std::this_thread::get_id() != frontend.preparationOwner_)
            return Result<bool>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        static_cast<void>(frontend.resourceRegistry_->DrainRetirements());
        return frontend.resourceRegistry_->ExternalRetirementComplete(Identity(texture));
    }
}  // namespace Horo::Render::Detail
