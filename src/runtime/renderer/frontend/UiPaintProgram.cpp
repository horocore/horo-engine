#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "UiPaintProgramInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Render {
    namespace UiPaintProgramErrors {
        const ErrorCodeDescriptor PreparationRequired{ErrorDomainId{"horo.render"},
                                                      ErrorCode{"render.ui_paint.program.preparation_required"}, ErrorSeverity::Info,
                                                      "The current source requires a complete replacement UI paint preparation.",
                                                      "Coalesce the newest canonical source at the explicit structural safe point; never "
                                                      "submit a revoked source."};
        const ErrorCodeDescriptor
            Capacity{ErrorDomainId{"horo.render"}, ErrorCode{"render.ui_paint.program.capacity"}, ErrorSeverity::Error,
                     "The complete UI paint program exceeds its admitted finite preparation.",
                     "Increase explicit host bounds or reduce the source; no mask, group or draw may be silently omitted."};
    }  // namespace UiPaintProgramErrors

    /** @copydoc UiPaintProgram::UiPaintProgram */
    UiPaintProgram::UiPaintProgram(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiPaintProgram::~UiPaintProgram() {
        static_cast<void>(Shutdown());
    }

    /** @copydoc UiPaintProgram::Create */
    Result<std::unique_ptr<UiPaintProgram>> UiPaintProgram::Create(RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor,
                                                                   const Runtime::Ui::UiRenderSnapshot &prototype,
                                                                   std::span<const RenderTextureHandle> textures,
                                                                   const CookedUiPaintKernel &kernel,
                                                                   const CancellationToken &cancellation) {
        using Creation = Result<std::unique_ptr<UiPaintProgram>>;
        if (const auto admitted = renderer.RequireOpenOwner(); admitted.HasError())
            return Creation::Failure(admitted.ErrorValue());
        if (!prototype.IsValid() || !descriptor.projection.IsValid() || !descriptor.capacity.IsValid() || descriptor.maximumPasses == 0 ||
            descriptor.maximumPasses > RenderGraphLimits::HardMaxPasses || textures.size() > UiPaintLimits::MaximumSampledResources)
            return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        try {
            auto storage = std::make_unique<Storage>(prototype, descriptor);
            storage->source.external.assign(textures.begin(), textures.end());
            for (std::size_t index = 0; index < textures.size(); ++index) {
                if (!textures[index].IsValid() || std::ranges::find(textures.first(index), textures[index]) != textures.begin() + index)
                    return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
                if (const auto state = renderer.ResourceState(textures[index]); state.HasError())
                    return Creation::Failure(state.ErrorValue());
            }
            if (const auto roles = storage->BuildRoles(); roles.HasError())
                return Creation::Failure(roles.ErrorValue());
            auto candidate = std::unique_ptr<UiPaintProgram>(new UiPaintProgram(std::move(storage)));
            auto &prepared = *candidate->storage_;
            if (const auto native = candidate->PrepareNative(renderer, kernel, cancellation); native.HasError())
                return Creation::Failure(native.ErrorValue());
            prepared.identity = std::make_shared<Detail::UiPaintProgramIdentity>();
            return Creation::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Creation::Failure(MakeError(UiPaintProgramErrors::Capacity));
        }
    }

    /** @copydoc UiPaintProgram::PrepareNative */
    Result<void> UiPaintProgram::PrepareNative(RenderFrontend &renderer, const CookedUiPaintKernel &kernel,
                                               const CancellationToken &cancellation) {
        auto &prepared = *storage_;
        auto &native = prepared.native;
        const auto &descriptor = prepared.descriptor;
        auto buffers = UiPaintFrameBufferPool::Create(renderer, descriptor.capacity, descriptor.frameSlots);
        if (buffers.HasError())
            return Result<void>::Failure(buffers.ErrorValue());
        native.buffers = std::move(buffers).Value();
        prepared.frontend = renderer.uiPaintFrontendState_;
        auto primary = renderer.RealizeUiPaintKernel(kernel);
        if (primary.HasError())
            return Result<void>::Failure(primary.ErrorValue());
        native.primary = std::move(primary).Value();
        auto offscreen = kernel;
        offscreen.colorFormat = RenderTextureFormat::Rgba16Float;
        auto isolated = renderer.RealizeUiPaintKernel(offscreen);
        if (isolated.HasError())
            return Result<void>::Failure(isolated.ErrorValue());
        native.offscreen = std::move(isolated).Value();
        native.slots.resize(descriptor.frameSlots);
        for (std::uint32_t slot = 0; slot < descriptor.frameSlots; ++slot) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
            if (const auto ready = prepared.CreateSlot(renderer, slot, native.buffers->slots_[slot], kernel.stencilFormat);
                ready.HasError())
                return ready;
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::TextureResource */
    Result<RenderGraphResourceId> UiPaintProgram::TextureResource(std::uint32_t slot, RenderTextureHandle texture) const {
        using Mapping = Result<RenderGraphResourceId>;
        if (std::this_thread::get_id() != storage_->owner || storage_->closed || slot >= storage_->native.slots.size())
            return Mapping::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (storage_->frontend.expired())
            return Mapping::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        const auto found = std::ranges::find(storage_->source.external, texture);
        if (found == storage_->source.external.end())
            return Mapping::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        const auto index = static_cast<std::size_t>(found - storage_->source.external.begin());
        return Mapping::Success(storage_->native.slots[slot].textures[index]);
    }

    /** @copydoc UiPaintProgram::Prepare */
    Result<UiPaintPreparedFrame> UiPaintProgram::Prepare(std::uint32_t slotIndex, const UiRenderSubmission &source,
                                                         const CancellationToken &cancellation) {
        using Preparation = Result<UiPaintPreparedFrame>;
        auto &state = *storage_;
        if (std::this_thread::get_id() != state.owner || state.closed || slotIndex >= state.native.slots.size())
            return Preparation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto owner = state.frontend.lock();
        if (!owner || !owner->frontend)
            return Preparation::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (const auto valid = state.ValidateSource(source); valid.HasError())
            return Preparation::Failure(valid.ErrorValue());
        auto &slot = state.native.slots[slotIndex];
        if (const auto validated = state.validation.Validate(source, slot.graph->Resources()); validated.HasError())
            return Preparation::Failure(validated.ErrorValue());
        for (const auto &surface : slot.surfaces)
            if (const auto ready = owner->frontend->ResourceOperationResult(surface.operation); ready.HasError())
                return Preparation::Failure(ready.ErrorValue());
        if (const auto ready = owner->frontend->ResourceOperationResult(slot.stencil.operation); ready.HasError())
            return Preparation::Failure(ready.ErrorValue());
        for (const auto &image : source.images)
            if (const auto ready = owner->frontend->ResourceOperationResult(image.realization->Creation().operation); ready.HasError())
                return Preparation::Failure(ready.ErrorValue());
        for (const auto &font : source.fonts)
            if (const auto ready = owner->frontend->ResourceOperationResult(font.realization->Creation().operation); ready.HasError())
                return Preparation::Failure(ready.ErrorValue());
        for (const auto handle : state.source.external) {
            const auto current = owner->frontend->ResourceState(handle);
            if (current.HasError())
                return Preparation::Failure(current.ErrorValue());
            if (current.Value() != RenderResourceState::Ready)
                return Preparation::Failure(MakeError(UiPaintFrameErrors::Pending));
        }
        auto &candidate = slot.frames[1 - slot.accepted];
        if (const auto built = state.BuildFrame(slot, candidate, source); built.HasError())
            return Preparation::Failure(built.ErrorValue());
        const auto uploaded =
            state.native.buffers->Update(slotIndex, candidate.vertices, candidate.indices, candidate.parameters, cancellation);
        if (uploaded.HasError())
            return Preparation::Failure(uploaded.ErrorValue());
        slot.accepted = 1 - slot.accepted;
        slot.revision = uploaded.Value().revision;
        state.Publish(slot, candidate, slot.revision);
        return Preparation::Success(UiPaintPreparedFrame{state.identity, slotIndex, slot.revision});
    }

    /** @copydoc UiPaintProgram::Submit */
    Result<void> UiPaintProgram::Submit(RenderFrameScope &frame, const UiPaintPreparedFrame &prepared, UiRenderSubmission source) {
        auto &state = *storage_;
        if (std::this_thread::get_id() != state.owner || state.closed || prepared.identity_.lock() != state.identity ||
            prepared.slot_ >= state.native.slots.size())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (state.frontend.expired())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        auto &slot = state.native.slots[prepared.slot_];
        const auto &accepted = slot.frames[slot.accepted];
        if (prepared.revision_ != slot.revision || !source.geometry || !source.geometry->IsValid() || !accepted.geometry ||
            &source.geometry->Descriptor() != &accepted.geometry->Descriptor() || !accepted.evidence.Matches(source))
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        if (const auto valid = state.ValidateSource(source); valid.HasError())
            return valid;
        if (const auto valid = state.validation.Validate(source, slot.graph->Resources()); valid.HasError())
            return valid;
        return frame.ComposeUi(*slot.graph, slot.workloads, std::move(source));
    }

    /** @copydoc UiPaintProgram::Shutdown */
    Result<void> UiPaintProgram::Shutdown() {
        return storage_->Shutdown();
    }

    /** @copydoc UiPaintProgram::Storage::Shutdown */
    Result<void> UiPaintProgram::Storage::Shutdown() {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        closed = true;
        identity.reset();
        const auto state = frontend.lock();
        for (auto &slot : native.slots) {
            for (auto &surface : slot.surfaces) {
                if (!surface.handle.IsValid())
                    continue;
                if (state && state->frontend) {
                    if (const auto released = state->frontend->ReleaseTexture(surface.handle); released.HasError())
                        return released;
                }
                surface.handle = {};
            }
            if (slot.stencil.handle.IsValid()) {
                if (state && state->frontend) {
                    if (const auto released = state->frontend->ReleaseTexture(slot.stencil.handle); released.HasError())
                        return released;
                }
                slot.stencil.handle = {};
            }
        }
        return native.buffers ? native.buffers->Shutdown() : Result<void>::Success();
    }
}  // namespace Horo::Render
