#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "RenderFrontendErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <new>

namespace Horo::Render {
    namespace Detail {
        /** @brief Inert owner-incarnation marker; no frontend pointer or native authority survives revocation. */
        struct VfxFrontendState final {};
    }  // namespace Detail

    namespace {
        /** @brief Selects one dedicated instance binding without borrowing mutable frontend metadata. */
        Result<MaterialBufferBinding> VfxInstanceBinding(const MaterialBindingDescriptor &descriptor) {
            using Binding = Result<MaterialBufferBinding>;
            const auto resource =
                std::ranges::find(descriptor.resources, VfxShaderParameterIds::Instances, &MaterialResourceBinding::binding);
            if (resource == descriptor.resources.end() || resource->arrayElement != 0)
                return Binding::Failure(MakeError(VfxFrameErrors::InvalidInput));
            const auto *binding = std::get_if<MaterialBufferBinding>(&resource->value);
            if (!binding || binding->offset != 0)
                return Binding::Failure(MakeError(VfxFrameErrors::InvalidInput));
            for (const auto &other : descriptor.resources)
                if (&other != &*resource)
                    if (const auto *alias = std::get_if<MaterialBufferBinding>(&other.value); alias && alias->buffer == binding->buffer)
                        return Binding::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return Binding::Success(*binding);
        }

        /** @brief Resets owner-thread mutation admission on every return/exception without allocating. */
        class VfxMutationScope final {
        public:
            explicit VfxMutationScope(bool &updating) noexcept : updating_(updating) {
                updating_ = true;
            }

            ~VfxMutationScope() {
                updating_ = false;
            }

            VfxMutationScope(const VfxMutationScope &) = delete;
            VfxMutationScope &operator=(const VfxMutationScope &) = delete;

        private:
            bool &updating_;
        };
    }  // namespace

    /** @copydoc RenderFrontend::ValidateVfxResidents */
    Result<void> RenderFrontend::ValidateVfxResidents(const VfxResidentProof &proof) const {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_ || proof.buffer.owner != resourceRegistry_->Owner() || proof.buffer.slot >= buffers_.size() ||
            proof.deviceIncarnation == 0 || proof.deviceIncarnation != backend_->Capabilities().support.deviceIncarnation)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto currentBuffer = BackendInstance(proof.buffer);
        const auto currentPipeline = BackendInstance(proof.pipeline);
        if (currentBuffer.HasError())
            return Result<void>::Failure(currentBuffer.ErrorValue());
        if (currentPipeline.HasError())
            return Result<void>::Failure(currentPipeline.ErrorValue());
        const auto &current = buffers_[proof.buffer.slot];
        if (currentBuffer.Value() != proof.bufferInstance || currentPipeline.Value() != proof.pipelineInstance ||
            current.generation != proof.buffer.generation || current.descriptor.byteSize != proof.byteSize ||
            current.descriptor.usage != RenderBufferUsage::Uniform || current.descriptor.access != RenderBufferAccess::HostVisible)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::InspectVfxSlotMaterial */
    Result<RenderFrontend::VfxSlotPreparation> RenderFrontend::InspectVfxSlotMaterial(const ResidentMaterialBinding &material) const {
        const auto &descriptor = material.Descriptor();
        const auto semanticBytes = VfxShaderParameterBytes(descriptor.layout);
        if (semanticBytes.HasError())
            return Result<VfxSlotPreparation>::Failure(semanticBytes.ErrorValue());
        const auto selected = VfxInstanceBinding(descriptor);
        if (selected.HasError())
            return Result<VfxSlotPreparation>::Failure(selected.ErrorValue());
        const auto binding = selected.Value();
        if (binding.buffer.owner != resourceRegistry_->Owner() || binding.buffer.slot >= buffers_.size())
            return Result<VfxSlotPreparation>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        // A selected-adapter query may reenter the owner and resize/release registry metadata.
        // Only plain facts, never a reference into buffers_, may survive that callback boundary.
        const auto bufferHandle = binding.buffer;
        const auto bufferBytes = buffers_[binding.buffer.slot].descriptor.byteSize;
        const auto boundBytes = binding.byteCount;
        const auto resident = BackendInstance(binding.buffer);
        const auto pipeline = BackendInstance(descriptor.pipeline);
        if (resident.HasError())
            return Result<VfxSlotPreparation>::Failure(resident.ErrorValue());
        if (pipeline.HasError())
            return Result<VfxSlotPreparation>::Failure(pipeline.ErrorValue());
        const VfxResidentProof proof{bufferHandle,     descriptor.pipeline, resident.Value(),
                                     pipeline.Value(), bufferBytes,         backend_->Capabilities().support.deviceIncarnation};
        if (const auto valid = ValidateVfxResidents(proof); valid.HasError())
            return Result<VfxSlotPreparation>::Failure(valid.ErrorValue());
        return Result<VfxSlotPreparation>::Success({proof, semanticBytes.Value(), boundBytes});
    }

    /** @copydoc RenderFrontend::ExistingVfxFrameSlot */
    Result<std::shared_ptr<PreparedVfxFrameSlot>> RenderFrontend::ExistingVfxFrameSlot(const ResidentMaterialBinding &material,
                                                                                       const RenderBufferHandle bufferHandle) const {
        using Preparation = Result<std::shared_ptr<PreparedVfxFrameSlot>>;
        auto existing = buffers_[bufferHandle.slot].vfxSlot.lock();
        if (!existing || existing->owner_.lock() != vfxFrontendState_ || !vfxFrontendState_ || existing->buffer_ != bufferHandle ||
            existing->material_.get() != &material)
            return Preparation::Failure(MakeError(VfxFrameErrors::InvalidInput, "This buffer generation already has a VFX slot claim."));
        if (existing->updating_ || existing->drawPreparation_)
            return Preparation::Failure(MakeError(VfxFrameErrors::Pending, "This VFX slot is being published."));
        return Preparation::Success(std::move(existing));
    }

    /** @copydoc RenderFrontend::IssueVfxFrameSlot */
    Result<std::shared_ptr<PreparedVfxFrameSlot>> RenderFrontend::IssueVfxFrameSlot(std::shared_ptr<const ResidentMaterialBinding> material,
                                                                                    const VfxResidentProof &proof,
                                                                                    const std::size_t nativeBytes) {
        using Preparation = Result<std::shared_ptr<PreparedVfxFrameSlot>>;
        const auto bufferHandle = proof.buffer;
        if (const auto valid = ValidateVfxResidents(proof); valid.HasError())
            return Preparation::Failure(valid.ErrorValue());
        const auto &current = buffers_[bufferHandle.slot];
        if (!current.vfxPreparing || current.vfxClaimed || !current.vfxSlot.expired())
            return Preparation::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (!vfxFrontendState_)
            vfxFrontendState_ = std::make_shared<const Detail::VfxFrontendState>();
        auto prepared = std::shared_ptr<PreparedVfxFrameSlot>(
            new PreparedVfxFrameSlot{vfxFrontendState_, std::move(material), bufferHandle, nativeBytes, proof.deviceIncarnation});
        buffers_[bufferHandle.slot].vfxSlot = prepared;
        buffers_[bufferHandle.slot].vfxClaimed = true;
        return Preparation::Success(std::move(prepared));
    }

    /** @copydoc RenderFrontend::PrepareVfxFrameSlot */
    Result<std::shared_ptr<PreparedVfxFrameSlot>> RenderFrontend::PrepareVfxFrameSlot(
        std::shared_ptr<const ResidentMaterialBinding> material) {
        using Preparation = Result<std::shared_ptr<PreparedVfxFrameSlot>>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Preparation::Failure(owner.ErrorValue());
        if (activeFrameScope_ || !material || material->Id().owner != resourceRegistry_->Owner())
            return Preparation::Failure(MakeError(VfxFrameErrors::InvalidInput));
        const auto selected = InspectVfxSlotMaterial(*material);
        if (selected.HasError())
            return Preparation::Failure(selected.ErrorValue());
        const auto facts = selected.Value();
        const auto bufferHandle = facts.residents.buffer;
        if (buffers_[bufferHandle.slot].vfxPreparing)
            return Preparation::Failure(MakeError(VfxFrameErrors::Pending, "This buffer's VFX slot is already being prepared."));
        if (buffers_[bufferHandle.slot].vfxClaimed)
            return ExistingVfxFrameSlot(*material, bufferHandle);
        buffers_[bufferHandle.slot].vfxPreparing = true;
        // Index/generation cleanup survives buffer-vector growth during an adapter callback.
        const auto clearPreparation = [this](const RenderBufferHandle *handle) noexcept {
            if (handle->slot < buffers_.size() && buffers_[handle->slot].generation == handle->generation)
                buffers_[handle->slot].vfxPreparing = false;
        };
        std::unique_ptr<const RenderBufferHandle, decltype(clearPreparation)> preparation{&bufferHandle, clearPreparation};
        try {
            const auto nativeBytes = backend_->QueryMaterialParameterBufferSize(facts.residents.pipelineInstance, material->Descriptor(),
                                                                                VfxShaderParameterIds::Instances);
            if (nativeBytes.HasError())
                return Preparation::Failure(nativeBytes.ErrorValue());
            if (nativeBytes.Value() < facts.semanticBytes || nativeBytes.Value() > MaterialBindingLimits{}.maximumParameterBytes ||
                facts.boundBytes != nativeBytes.Value() || facts.residents.byteSize != nativeBytes.Value())
                return Preparation::Failure(MakeError(VfxFrameErrors::InvalidInput));
            return IssueVfxFrameSlot(std::move(material), facts.residents, nativeBytes.Value());
        } catch (const std::bad_alloc &) {
            return Preparation::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted, "VFX slot preparation allocation failed."));
        } catch (...) {
            return Preparation::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native VFX slot preparation threw."));
        }
    }

    /** @copydoc RenderFrontend::UpdateVfxFrame */
    Result<void> RenderFrontend::UpdateVfxFrame(const std::shared_ptr<PreparedVfxFrameSlot> &slot, const std::uint64_t revision,
                                                const std::span<const VfxPackedDrawInstance> instances,
                                                const CancellationToken &cancellation) {
        return UpdateVfxFrameInternal(slot, revision, instances, cancellation, nullptr);
    }

    /** @copydoc RenderFrontend::AdmitVfxFrameMutation */
    Result<void> RenderFrontend::AdmitVfxFrameMutation(const std::shared_ptr<PreparedVfxFrameSlot> &slot, const std::uint64_t revision,
                                                       const VfxDrawOrder *drawIssuer) {
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return owner;
        if (activeFrameScope_ || !slot || slot->owner_.lock() != vfxFrontendState_ || !vfxFrontendState_ || revision == 0 ||
            revision <= slot->revision_)
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        if (slot->updating_ || slot->drawPreparation_ != drawIssuer)
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending, "This VFX slot is already being published."));
        if (!drawIssuer)
            ReleaseUnborrowedVfxDraws();
        // A draw transaction owns one prepared-record reference in addition to its admitted unique host handle.
        if ((!drawIssuer && slot.use_count() != 1) || (drawIssuer && slot.use_count() > 2))
            return Result<void>::Failure(MakeError(VfxFrameErrors::Pending, "VFX publication requires unique shared-handle ownership."));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::UpdateVfxFrameInternal */
    Result<void> RenderFrontend::UpdateVfxFrameInternal(const std::shared_ptr<PreparedVfxFrameSlot> &slot, const std::uint64_t revision,
                                                        const std::span<const VfxPackedDrawInstance> instances,
                                                        const CancellationToken &cancellation, const VfxDrawOrder *drawIssuer) {
        if (const auto admitted = AdmitVfxFrameMutation(slot, revision, drawIssuer); admitted.HasError())
            return admitted;
        // Retain across the native callback even if it releases the caller's handle by reference.
        const auto retained = slot;
        VfxMutationScope mutation{retained->updating_};
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(VfxFrameErrors::Cancelled));
        const auto buffer = BackendInstance(retained->buffer_);
        const auto &descriptor = retained->material_->Descriptor();
        const auto pipeline = BackendInstance(descriptor.pipeline);
        if (buffer.HasError())
            return Result<void>::Failure(buffer.ErrorValue());
        if (pipeline.HasError())
            return Result<void>::Failure(pipeline.ErrorValue());
        const VfxResidentProof proof{retained->buffer_, descriptor.pipeline,       buffer.Value(),
                                     pipeline.Value(),  retained->staging_.size(), retained->deviceIncarnation_};
        if (const auto valid = ValidateVfxResidents(proof); valid.HasError())
            return valid;
        const auto packed = PackVfxShaderParameters(descriptor.layout, instances, retained->staging_);
        if (packed.HasError())
            return Result<void>::Failure(packed.ErrorValue());
        std::fill(retained->staging_.begin() + packed.Value(), retained->staging_.end(), std::byte{0});
        return PublishVfxSlot(retained, proof, revision, static_cast<std::uint32_t>(instances.size()), cancellation);
    }

    /** @copydoc RenderFrontend::PublishVfxSlot */
    Result<void> RenderFrontend::PublishVfxSlot(const std::shared_ptr<PreparedVfxFrameSlot> &retained, const VfxResidentProof &proof,
                                                const std::uint64_t revision, const std::uint32_t instanceCount,
                                                const CancellationToken &cancellation) {
        const auto &descriptor = retained->material_->Descriptor();
        VfxFrameWriteStatus writeStatus{VfxFrameWriteStatus::NotStarted};
        const NativeVfxFrameUpdate update{proof.bufferInstance,
                                          proof.pipelineInstance,
                                          &retained->material_->BackendBinding().NativeBinding(),
                                          &descriptor.layout,
                                          revision,
                                          instanceCount,
                                          retained->staging_,
                                          cancellation,
                                          &writeStatus,
                                          retained->deviceIncarnation_};
        try {
            const auto uploaded = backend_->UpdateVfxFrame(update);
            const auto valid = ValidateVfxResidents(proof);
            if (valid.HasError() || retained->owner_.lock() != vfxFrontendState_ || !vfxFrontendState_) {
                retained->published_ = false;
                if (uploaded.HasError())
                    return uploaded;
                return valid.HasError() ? valid : Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
            }
            if (uploaded.HasError()) {
                if (writeStatus != VfxFrameWriteStatus::NotStarted)
                    retained->published_ = false;
                return uploaded;
            }
            if (writeStatus != VfxFrameWriteStatus::Committed) {
                retained->published_ = false;
                return Result<void>::Failure(
                    MakeError(VfxFrameErrors::InvalidInput, "Native VFX owner omitted write completion evidence."));
            }
            retained->revision_ = revision;
            retained->instanceCount_ = instanceCount;
            retained->capture_.reset();
            retained->order_.reset();
            retained->published_ = true;
            return Result<void>::Success();
        } catch (...) {
            retained->published_ = false;
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceBackendException, "Native VFX frame publication threw."));
        }
    }
}  // namespace Horo::Render
