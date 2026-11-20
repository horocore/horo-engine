#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "UiPaintProgramInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Render {
    /** @copydoc Detail::RenderFrontendResourceAccess::MatchesSceneCanvas */
    bool Detail::RenderFrontendResourceAccess::MatchesSceneCanvas(const UiPaintProgram &program, const UiPaintProgramDescriptor &policy,
                                                                  const Runtime::Ui::UiRenderSnapshot &prototype,
                                                                  const std::span<const RenderTextureHandle> textures) noexcept {
        const auto &state = *program.storage_;
        const auto &current = state.descriptor;
        const auto &a = current.capacity;
        const auto &b = policy.capacity;
        return state.owner == std::this_thread::get_id() && !state.closed && state.target == UiPaintRootTarget::SceneCanvas &&
               current.projection == policy.projection && current.frameSlots == policy.frameSlots &&
               current.maximumPasses == policy.maximumPasses && a.vertices == b.vertices && a.indices == b.indices &&
               a.parameters == b.parameters && std::ranges::equal(textures, state.source.external) && state.MatchesSource(prototype);
    }

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

    namespace {
        /** @brief Abandons a private CPU candidate on any failed update without releasing the still-accepted publication. */
        class CandidateSourceGuard final {
        public:
            CandidateSourceGuard(Detail::UiPaintCpuFrame &frame, bool &preparing) noexcept : frame_(&frame), preparing_(preparing) {
                preparing_ = true;
            }

            ~CandidateSourceGuard() {
                if (frame_)
                    frame_->ReleaseSource();
                preparing_ = false;
            }

            void Commit() noexcept {
                frame_ = nullptr;
            }

            CandidateSourceGuard(const CandidateSourceGuard &) = delete;
            CandidateSourceGuard &operator=(const CandidateSourceGuard &) = delete;

        private:
            Detail::UiPaintCpuFrame *frame_;
            bool &preparing_;
        };

        /** @brief Records all creation failures before any explicit shutdown release can recycle a generation. */
        void ObserveSlotCreations(Detail::UiPaintSlotProgram &slot, const RenderFrontend &frontend) {
            for (auto &surface : slot.surfaces)
                if (surface.handle.IsValid())
                    static_cast<void>(Detail::ObserveUiPaintCreation(frontend, surface.operation, surface.handle));
            if (slot.stencil.handle.IsValid())
                static_cast<void>(Detail::ObserveUiPaintCreation(frontend, slot.stencil.operation, slot.stencil.handle));
            if (slot.neutralSample.handle.IsValid())
                static_cast<void>(Detail::ObserveUiPaintCreation(frontend, slot.neutralSample.operation, slot.neutralSample.handle));
        }

        /** @brief Clears one local handle only after explicit release succeeds or frontend revocation has removed native authority. */
        Result<void> ReleaseSlotTexture(ResourceCreation<RenderTextureHandle> &texture, RenderFrontend *frontend) {
            if (!texture.handle.IsValid())
                return Result<void>::Success();
            if (frontend) {
                if (const auto released = frontend->ReleaseTexture(texture.handle); released.HasError())
                    return released;
            }
            texture.handle = {};
            return Result<void>::Success();
        }

        /** @brief Preserves unreleased slot handles for an explicit owner-thread Shutdown retry. */
        Result<void> ReleaseSlotTextures(Detail::UiPaintSlotProgram &slot, RenderFrontend *frontend) {
            for (auto &surface : slot.surfaces)
                if (const auto released = ReleaseSlotTexture(surface, frontend); released.HasError())
                    return released;
            if (const auto released = ReleaseSlotTexture(slot.stencil, frontend); released.HasError())
                return released;
            return ReleaseSlotTexture(slot.neutralSample, frontend);
        }
    }  // namespace

    /** @copydoc UiPaintProgram::UiPaintProgram */
    UiPaintProgram::UiPaintProgram(ConstructionKey, std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    UiPaintProgram::~UiPaintProgram() {
        storage_->RetireNative();
    }

    /** @copydoc UiPaintProgram::Create */
    Result<std::unique_ptr<UiPaintProgram>> UiPaintProgram::Create(RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor,
                                                                   const Runtime::Ui::UiRenderSnapshot &prototype,
                                                                   std::span<const RenderTextureHandle> textures,
                                                                   const CookedUiPaintKernel &kernel,
                                                                   const CancellationToken &cancellation) {
        return CreateForTarget(renderer, descriptor, prototype, textures, kernel, cancellation, Detail::UiPaintRootTarget::PrimaryOutput);
    }

    /** @copydoc UiPaintProgram::CreateCanvasSurface */
    Result<std::unique_ptr<UiPaintProgram>> UiPaintProgram::CreateCanvasSurface(
        RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
        std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const CancellationToken &cancellation) {
        return CreateForTarget(renderer, descriptor, prototype, textures, kernel, cancellation, Detail::UiPaintRootTarget::CanvasSurface);
    }

    /** @copydoc UiPaintProgram::CreateSceneCanvas */
    Result<std::unique_ptr<UiPaintProgram>> UiPaintProgram::CreateSceneCanvas(
        RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
        std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const UiPaintSceneCanvasGraph &scene,
        const CancellationToken &cancellation) {
        if (!scene.author)
            return Result<std::unique_ptr<UiPaintProgram>>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        try {
            const Detail::UiPaintSceneCanvasRecipe recipe{scene.limits, scene.graphicsQueue,
                                                          [&scene](RenderGraphBuilder &builder, const UiPaintSceneCanvasSource &source,
                                                                   std::vector<RenderGraphImportedState> &initial, auto &, auto &) {
                return scene.author(builder, source, initial);
            }};
            return Detail::RenderFrontendResourceAccess::CreateSceneCanvas(renderer, descriptor, prototype, textures, kernel, recipe,
                                                                           cancellation);
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<UiPaintProgram>>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        }
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::CreateSceneCanvas */
    Result<std::unique_ptr<UiPaintProgram>> Detail::RenderFrontendResourceAccess::CreateSceneCanvas(
        RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
        std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const UiPaintSceneCanvasRecipe &scene,
        const CancellationToken &cancellation) {
        if (!scene.author || !scene.graphicsQueue.IsValid() || !scene.limits.IsValid() || scene.limits.maxPasses > descriptor.maximumPasses)
            return Result<std::unique_ptr<UiPaintProgram>>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        return UiPaintProgram::CreateForTarget(renderer, descriptor, prototype, textures, kernel, cancellation,
                                               UiPaintRootTarget::SceneCanvas, &scene);
    }

    /** @copydoc Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas */
    Result<const CompiledRenderGraphExecution *> Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(
        UiPaintProgram &program, const std::uint32_t index, const std::span<const RenderGraphExternalTextureBinding> bindings,
        const std::span<const RenderGraphOptionalRead> reads) {
        using Outcome = Result<const CompiledRenderGraphExecution *>;
        auto &state = *program.storage_;
        if (state.owner != std::this_thread::get_id() || state.closed || index >= state.native.slots.size())
            return Outcome::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto owner = state.frontend.lock();
        if (!owner || !owner->frontend)
            return Outcome::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (const auto boundary = RequirePreparationBoundary(*owner->frontend); boundary.HasError())
            return Outcome::Failure(boundary.ErrorValue());
        auto &slot = state.native.slots[index];
        if (!slot.CanBindScene())
            return Outcome::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        slot.boundScene = nullptr;
        auto bound = slot.sceneTemplate->Bind(*owner->frontend, 0, bindings, reads);
        if (bound.HasValue()) {
            slot.boundScene = bound.Value();
            slot.paintRevisionAtBind = slot.revision;
        }
        return bound;
    }

    /** @copydoc UiPaintProgram::CreateForTarget */
    Result<std::unique_ptr<UiPaintProgram>> UiPaintProgram::CreateForTarget(
        RenderFrontend &renderer, const UiPaintProgramDescriptor &descriptor, const Runtime::Ui::UiRenderSnapshot &prototype,
        std::span<const RenderTextureHandle> textures, const CookedUiPaintKernel &kernel, const CancellationToken &cancellation,
        const Detail::UiPaintRootTarget target, const Detail::UiPaintSceneCanvasRecipe *scene) {
        using Creation = Result<std::unique_ptr<UiPaintProgram>>;
        if (const auto admitted = renderer.RequireOpenOwner(); admitted.HasError())
            return Creation::Failure(admitted.ErrorValue());
        if (!prototype.IsValid() || !descriptor.projection.IsValid() || !descriptor.capacity.IsValid() || descriptor.maximumPasses == 0 ||
            descriptor.maximumPasses > RenderGraphLimits::HardMaxPasses || textures.size() > UiPaintLimits::MaximumSampledResources)
            return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        try {
            auto storage = std::make_unique<Storage>(prototype, descriptor, target);
            storage->source.external.assign(textures.begin(), textures.end());
            for (std::size_t index = 0; index < textures.size(); ++index) {
                if (const auto prefix = textures.first(index);
                    !textures[index].IsValid() || std::ranges::find(prefix, textures[index]) != prefix.end())
                    return Creation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
                if (const auto state = renderer.ResourceState(textures[index]); state.HasError())
                    return Creation::Failure(state.ErrorValue());
            }
            if (const auto roles = storage->BuildRoles(); roles.HasError())
                return Creation::Failure(roles.ErrorValue());
            auto candidate = std::make_unique<UiPaintProgram>(ConstructionKey{}, std::move(storage));
            auto &prepared = *candidate->storage_;
            if (const auto native = candidate->PrepareNative(renderer, kernel, cancellation, scene); native.HasError())
                return Creation::Failure(native.ErrorValue());
            prepared.identity = std::make_shared<Detail::UiPaintProgramIdentity>();
            return Creation::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Creation::Failure(MakeError(UiPaintProgramErrors::Capacity));
        }
    }

    /** @copydoc UiPaintProgram::PrepareNative */
    Result<void> UiPaintProgram::PrepareNative(RenderFrontend &renderer, const CookedUiPaintKernel &kernel,
                                               const CancellationToken &cancellation, const Detail::UiPaintSceneCanvasRecipe *scene) {
        auto &prepared = *storage_;
        auto &native = prepared.native;
        const auto &descriptor = prepared.descriptor;
        const auto prepareBuffers = [&](const bool exactSceneKernel) {
            auto buffers = UiPaintFrameBufferPool::CreateInternal(renderer, descriptor.capacity, descriptor.frameSlots, exactSceneKernel);
            if (buffers.HasError())
                return Result<void>::Failure(buffers.ErrorValue());
            native.buffers = std::move(buffers).Value();
            prepared.frontend = renderer.uiPaintFrontendState_;
            return Result<void>::Success();
        };
        if (!scene)
            if (const auto buffers = prepareBuffers(false); buffers.HasError())
                return buffers;
        if (prepared.target == Detail::UiPaintRootTarget::PrimaryOutput) {
            auto primary = renderer.RealizeUiPaintKernel(kernel);
            if (primary.HasError())
                return Result<void>::Failure(primary.ErrorValue());
            native.primary = std::move(primary).Value();
        }
        auto offscreen = kernel;
        offscreen.colorFormat = RenderTextureFormat::Rgba16Float;
        auto isolated = renderer.RealizeUiPaintKernel(offscreen);
        if (isolated.HasError())
            return Result<void>::Failure(isolated.ErrorValue());
        native.offscreen = std::move(isolated).Value();
        if (!scene && native.offscreen->RequiresGraphNeutralSample())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (scene)
            if (const auto buffers = prepareBuffers(true); buffers.HasError())
                return buffers;
        native.slots.resize(descriptor.frameSlots);
        for (std::uint32_t slot = 0; slot < descriptor.frameSlots; ++slot) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
            if (const auto ready = prepared.CreateSlot(renderer, slot, native.buffers->slots_[slot], kernel.stencilFormat, scene);
                ready.HasError())
                return ready;
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Cancelled));
        return Result<void>::Success();
    }

    /** @copydoc UiPaintProgram::CanvasSurface */
    Result<UiPaintCanvasSurface> UiPaintProgram::CanvasSurface(const UiPaintPreparedFrame &prepared) const {
        using Output = Result<UiPaintCanvasSurface>;
        const auto &state = *storage_;
        if (std::this_thread::get_id() != state.owner || state.closed || prepared.identity_.lock() != state.identity ||
            prepared.slot_ >= state.native.slots.size())
            return Output::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto owner = state.frontend.lock();
        if (!owner || !owner->frontend || state.target == Detail::UiPaintRootTarget::PrimaryOutput)
            return Output::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        const auto &slot = state.native.slots[prepared.slot_];
        if (prepared.revision_ != slot.revision)
            return Output::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        const auto color = slot.surfaces.back().handle;
        const auto resident = owner->frontend->ResourceState(color);
        if (resident.HasError())
            return Output::Failure(resident.ErrorValue());
        if (resident.Value() != RenderResourceState::Ready)
            return Output::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        const auto extent = state.descriptor.projection.pixelExtent;
        return Output::Success({color, {extent.width, extent.height}});
    }

    /** @copydoc UiPaintProgram::Inventory */
    UiPaintProgramInventory UiPaintProgram::Inventory() const noexcept {
        if (std::this_thread::get_id() != storage_->owner || storage_->closed || storage_->native.slots.empty())
            return {};
        auto inventory = storage_->native.slots.front().inventory;
        inventory.draws = storage_->descriptor.capacity.parameters;
        return inventory;
    }

    /** @copydoc UiPaintProgram::ValidatePreparation */
    Result<void> UiPaintProgram::ValidatePreparation() {
        if (std::this_thread::get_id() != storage_->owner || storage_->closed)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto owner = storage_->frontend.lock();
        if (!owner || !owner->frontend)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        std::optional<Error> failure;
        const auto observe = [&](ResourceOperationId operation, auto &handle) {
            if (const auto ready = Detail::ObserveUiPaintCreation(*owner->frontend, operation, handle); ready.HasError() && !failure)
                failure = ready.ErrorValue();
        };
        for (std::uint32_t slot = 0; slot < storage_->native.slots.size(); ++slot) {
            auto &pool = *storage_->native.buffers;
            const std::array handles{&pool.slots_[slot].vertices, &pool.slots_[slot].indices, &pool.slots_[slot].parameters};
            for (std::size_t index = 0; index < handles.size(); ++index)
                observe(pool.operations_[slot][index], *handles[index]);
            auto &resources = storage_->native.slots[slot];
            for (auto &surface : resources.surfaces)
                observe(surface.operation, surface.handle);
            observe(resources.stencil.operation, resources.stencil.handle);
            if (resources.neutralSample.handle.IsValid())
                observe(resources.neutralSample.operation, resources.neutralSample.handle);
        }
        if (failure)
            return Result<void>::Failure(std::move(*failure));
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
        return PrepareInternal(slotIndex, source, cancellation, nullptr);
    }

    /** @copydoc UiPaintProgram::PrepareSceneFrame */
    Result<UiPaintPreparedFrame> UiPaintProgram::PrepareSceneFrame(RenderFrameScope &frame, const std::uint32_t slot,
                                                                   const UiRenderSubmission &source,
                                                                   const CancellationToken &cancellation) {
        return PrepareInternal(slot, source, cancellation, &frame);
    }

    /** @copydoc UiPaintProgram::PrepareInternal */
    Result<UiPaintPreparedFrame> UiPaintProgram::PrepareInternal(const std::uint32_t slotIndex, const UiRenderSubmission &source,
                                                                 const CancellationToken &cancellation, const RenderFrameScope *frame) {
        using Preparation = Result<UiPaintPreparedFrame>;
        auto &state = *storage_;
        if (std::this_thread::get_id() != state.owner || state.closed || slotIndex >= state.native.slots.size())
            return Preparation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto owner = state.frontend.lock();
        if (!owner || !owner->frontend)
            return Preparation::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        if (!frame && owner->frontend->activeFrameScope_ != nullptr)
            return Preparation::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph, "Ordinary UI preparation is outside a frame."));
        FrameToken frameToken;
        if (frame) {
            if (state.target != Detail::UiPaintRootTarget::SceneCanvas || frame->owner_ != owner->frontend)
                return Preparation::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            if (const auto ready = frame->ValidateExecutionAdmission(); ready.HasError())
                return Preparation::Failure(ready.ErrorValue());
            frameToken = frame->Token();
        }
        if (const auto valid = state.ValidateSource(source); valid.HasError())
            return Preparation::Failure(valid.ErrorValue());
        auto &slot = state.native.slots[slotIndex];
        if (slot.preparing || slot.sceneExecuting)
            return Preparation::Failure(MakeError(UiPaintFrameErrors::Pending));
        const auto *graph = slot.Graph();
        if (!graph)
            return Preparation::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        if (const auto validated = state.validation.Validate(source, graph->Resources()); validated.HasError())
            return Preparation::Failure(validated.ErrorValue());
        for (const auto &surface : slot.surfaces)
            if (const auto ready = owner->frontend->ResourceOperationResult(surface.operation); ready.HasError())
                return Preparation::Failure(ready.ErrorValue());
        if (const auto ready = owner->frontend->ResourceOperationResult(slot.stencil.operation); ready.HasError())
            return Preparation::Failure(ready.ErrorValue());
        if (slot.neutralSample.handle.IsValid())
            if (const auto ready = owner->frontend->ResourceOperationResult(slot.neutralSample.operation); ready.HasError())
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
        CandidateSourceGuard candidateSource{candidate, slot.preparing};
        if (const auto built = state.BuildFrame(slot, candidate, source); built.HasError())
            return Preparation::Failure(built.ErrorValue());
        const auto uploaded = state.native.buffers->UpdateInternal(slotIndex, candidate.vertices, candidate.indices, candidate.parameters,
                                                                   cancellation, frameToken);
        if (uploaded.HasError())
            return Preparation::Failure(uploaded.ErrorValue());
        slot.accepted = 1 - slot.accepted;
        slot.revision = uploaded.Value().revision;
        state.Publish(slot, candidate, slot.revision);
        candidateSource.Commit();
        // The old prepared revision can no longer submit; its queued native captures retain their own exact provenance.
        slot.frames[1 - slot.accepted].ReleaseSource();
        return Preparation::Success(UiPaintPreparedFrame{state.identity, slotIndex, slot.revision, frameToken});
    }

    /** @copydoc UiPaintProgram::ValidateSubmission */
    Result<UiPaintGraphComposition> UiPaintProgram::ValidateSubmission(const UiPaintPreparedFrame &prepared, UiRenderSubmission source) {
        auto &state = *storage_;
        if (std::this_thread::get_id() != state.owner || state.closed || prepared.identity_.lock() != state.identity ||
            prepared.slot_ >= state.native.slots.size())
            return Result<UiPaintGraphComposition>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (state.frontend.expired())
            return Result<UiPaintGraphComposition>::Failure(MakeError(UiPaintFrameErrors::Unavailable));
        auto &slot = state.native.slots[prepared.slot_];
        if (const auto &accepted = slot.frames[slot.accepted];
            prepared.revision_ != slot.revision || !source.geometry || !source.geometry->IsValid() || !accepted.geometry ||
            &source.geometry->Descriptor() != &accepted.geometry->Descriptor() || !accepted.evidence.Matches(source))
            return Result<UiPaintGraphComposition>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        if (const auto valid = state.ValidateSource(source); valid.HasError())
            return Result<UiPaintGraphComposition>::Failure(valid.ErrorValue());
        const auto *graph = slot.Graph();
        if (!graph || (slot.sceneTemplate && slot.paintRevisionAtBind == prepared.revision_))
            return Result<UiPaintGraphComposition>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        if (const auto valid = state.validation.Validate(source, graph->Resources()); valid.HasError())
            return Result<UiPaintGraphComposition>::Failure(valid.ErrorValue());
        return Result<UiPaintGraphComposition>::Success({graph, slot.workloads, std::move(source), {}});
    }

    /** @copydoc UiPaintProgram::Submit */
    Result<void> UiPaintProgram::Submit(RenderFrameScope &frame, const UiPaintPreparedFrame &prepared, UiRenderSubmission source) {
        UiPaintProgramSubmission contribution{this, &prepared, std::move(source)};
        return SubmitBatch(frame, std::span{&contribution, 1});
    }

    /** @copydoc UiPaintProgram::ExecuteSceneCanvas */
    Result<void> UiPaintProgram::ExecuteSceneCanvas(RenderFrameScope &frame, const UiPaintPreparedFrame &prepared,
                                                    UiRenderSubmission source,
                                                    const std::span<const RenderGraphPassWorkload> sceneWorkloads) {
        if (storage_->target != Detail::UiPaintRootTarget::SceneCanvas || (prepared.frame_.IsValid() && prepared.frame_ != frame.Token()))
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        auto valid = ValidateSubmission(prepared, std::move(source));
        if (valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        auto accepted = std::move(valid).Value();
        const auto owner = storage_->frontend.lock();
        if (!owner || frame.owner_ != owner->frontend)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (const auto admitted = frame.ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        auto &slot = storage_->native.slots[prepared.slot_];
        const auto scenePasses = accepted.graph->Passes().subspan(accepted.workloads.size());
        if (slot.sceneExecuting || !std::ranges::equal(sceneWorkloads, scenePasses, std::ranges::equal_to{}, &RenderGraphPassWorkload::pass,
                                                       &RenderGraphExecutionPass::pass))
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        std::optional<RenderGraphQueueSubmission> submission;
        if (owner->frontend->Capabilities().supportsExplicitGraphSubmissions) {
            auto reserved = frame.ReserveSubmission(RenderQueueRole::Graphics);
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            submission.emplace(RenderGraphQueueSubmission{reserved.Value(), {0, slot.sceneExecution.size()}});
        }

        struct SceneExecutionScope final {
            Detail::UiPaintSlotProgram &slot;

            explicit SceneExecutionScope(Detail::UiPaintSlotProgram &owner) : slot(owner) {
                slot.sceneExecuting = true;
            }

            ~SceneExecutionScope() {
                std::ranges::fill(slot.sceneExecution, RenderGraphPassWorkload{});
                slot.sceneExecuting = false;
            }
        } scope{slot};

        const auto afterPaint = std::ranges::copy(accepted.workloads, slot.sceneExecution.begin()).out;
        std::ranges::copy(sceneWorkloads, afterPaint);
        const auto submissions =
            submission ? std::span<const RenderGraphQueueSubmission>{&*submission, 1} : std::span<const RenderGraphQueueSubmission>{};
        slot.boundScene = nullptr;
        return frame.ExecuteGraphInternal(*accepted.graph, slot.sceneExecution, &accepted.source, {}, submissions);
    }

    /** @copydoc UiPaintProgram::SubmitBatch */
    Result<void> UiPaintProgram::SubmitBatch(RenderFrameScope &frame, const std::span<UiPaintProgramSubmission> sources) {
        if (sources.empty() || sources.size() > 64)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        std::array<UiPaintGraphComposition, 64> canvases;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto &source = sources[index];
            if (!source.program || !source.prepared || source.program->storage_->target == Detail::UiPaintRootTarget::SceneCanvas ||
                std::ranges::any_of(sources.first(index), [&](const auto &previous) {
                return previous.program == source.program && previous.prepared && previous.prepared->slot_ == source.prepared->slot_;
            }))
                return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            auto validated = source.program->ValidateSubmission(*source.prepared, std::move(source.source));
            if (validated.HasError())
                return Result<void>::Failure(validated.ErrorValue());
            canvases[index] = std::move(validated).Value();
        }
        return frame.ComposeUi(std::span{canvases}.first(sources.size()));
    }

    /** @copydoc UiPaintProgram::Shutdown */
    Result<void> UiPaintProgram::Shutdown() {
        return storage_->Shutdown();
    }

    /** @copydoc UiPaintProgram::Storage::RetireNative */
    void UiPaintProgram::Storage::RetireNative() noexcept {
        closed = true;
        identity.reset();
        retirement->Request();
        if (std::this_thread::get_id() != owner)
            return;
        const auto state = frontend.lock();
        if (!state || !state->frontend)
            return;
        for (const auto &slot : native.slots) {
            for (const auto &surface : slot.surfaces)
                if (surface.handle.IsValid())
                    state->frontend->RetireOwnedUiPaintTexture(surface.handle, surface.operation);
            if (slot.stencil.handle.IsValid())
                state->frontend->RetireOwnedUiPaintTexture(slot.stencil.handle, slot.stencil.operation);
            if (slot.neutralSample.handle.IsValid())
                state->frontend->RetireOwnedUiPaintTexture(slot.neutralSample.handle, slot.neutralSample.operation);
        }
    }

    /** @copydoc UiPaintProgram::Storage::Shutdown */
    Result<void> UiPaintProgram::Storage::Shutdown() {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        if (std::ranges::any_of(native.slots, [](const auto &slot) {
            return slot.preparing || slot.sceneExecuting;
        }))
            return Result<void>::Failure(MakeError(UiPaintFrameErrors::Pending));
        closed = true;
        identity.reset();
        const auto state = frontend.lock();
        if (state && state->frontend)
            for (auto &slot : native.slots)
                ObserveSlotCreations(slot, *state->frontend);
        for (auto &slot : native.slots)
            if (const auto released = ReleaseSlotTextures(slot, state ? state->frontend : nullptr); released.HasError())
                return released;
        return native.buffers ? native.buffers->Shutdown() : Result<void>::Success();
    }
}  // namespace Horo::Render
