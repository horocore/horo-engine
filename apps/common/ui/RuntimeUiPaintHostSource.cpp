#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    namespace {
        /** @brief Validates a complete owned replacement inventory before any canonical source publication. */
        Result<std::vector<Render::RenderTextureHandle>> ResourceInventory(const RuntimeUiPaintResources &resources) {
            using Inventory = Result<std::vector<Render::RenderTextureHandle>>;
            if (resources.images.size() > Render::MaximumUiSubmissionImages || resources.fonts.size() > Render::MaximumUiSubmissionFonts ||
                resources.glyphs.size() > Render::MaximumUiSubmissionGlyphs || (!resources.fonts.empty() && !resources.atlas))
                return Inventory::Failure(MakeError(UiErrors::CapacityExceeded));
            std::vector<Render::RenderTextureHandle> result;
            result.reserve(resources.images.size() + resources.fonts.size());
            const auto append = [&](const Render::RenderTextureHandle texture) {
                if (std::ranges::find(result, texture) == result.end())
                    result.push_back(texture);
            };
            for (const auto &image : resources.images) {
                if (!image.registry || !image.realization.Creation().handle.IsValid())
                    return Inventory::Failure(MakeError(UiErrors::HandleMalformed));
                append(image.realization.Creation().handle);
            }
            for (const auto &font : resources.fonts) {
                if (!font.face || !font.realization.Creation().handle.IsValid())
                    return Inventory::Failure(MakeError(UiErrors::HandleMalformed));
                append(font.realization.Creation().handle);
            }
            return Inventory::Success(std::move(result));
        }
    }  // namespace

    /** @copydoc RuntimeUiPaintPort::AttachSource */
    Result<void> RuntimeUiPaintPort::AttachSource(RuntimeUiPaintSourcePreparation preparation) const {
        if (!state_ || !state_->Available() || state_->dispatching || state_->source || state_->phase != RuntimeUiPaintHostPhase::NoSource)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!state_->frontend->Capabilities().support.features.Supports(Render::RenderCapability::UiPaint))
            return Result<void>::Failure(MakeError(Render::UiPaintFrameErrors::Unavailable));
        if (preparation.extraction.view != preparation.geometry.view || preparation.program.frameSlots == 0)
            return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
        auto projection = RuntimeUiPaintProjection::Create(state_->service, preparation.instance, std::move(preparation.declaration),
                                                           preparation.extraction);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        auto arena = UiRenderGeometryArena::Create(preparation.geometry);
        if (arena.HasError())
            return Result<void>::Failure(arena.ErrorValue());
        auto owner = UiAnimationOwner::Create(state_->service, preparation.instance, preparation.allocator, std::move(preparation.registry),
                                              std::move(preparation.styles), std::move(preparation.animation), preparation.animationLimits);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        auto derived = Runtime::UiAnimationRuntimeParticipant::Compose(std::move(owner).Value(), state_->dispatch, preparation.runtime);
        if (derived.HasError())
            return Result<void>::Failure(derived.ErrorValue());
        auto candidate =
            std::make_unique<Detail::RuntimeUiPaintSource>(preparation.instance, std::move(derived).Value(), std::move(projection).Value(),
                                                           std::move(preparation.resources), std::move(arena).Value(),
                                                           Detail::RuntimeUiPaintSourcePolicy{preparation.extraction, preparation.geometry,
                                                                                              preparation.program,
                                                                                              std::move(preparation.kernel),
                                                                                              preparation.runtime.viewport});
        const auto resources = candidate->AdmitResources();
        if (resources.HasError())
            return resources;
        if (state_->started) {
            const auto started = candidate->animation->Startup(state_->cancellation.Token());
            if (started.HasError())
                return started;
        }
        state_->source = std::move(candidate);
        state_->phase = RuntimeUiPaintHostPhase::Active;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::Reload */
    Result<Runtime::UiAnimationRuntimeReload> RuntimeUiPaintPort::Reload(UiAnimationReloadPreparation preparation,
                                                                         RuntimeUiPaintDeclaration declaration,
                                                                         RuntimeUiPaintResources resources,
                                                                         const CancellationToken &cancellation) const {
        using Reloaded = Result<Runtime::UiAnimationRuntimeReload>;
        if (!state_ || !state_->Available() || state_->dispatching || !state_->source || state_->phase != RuntimeUiPaintHostPhase::Active ||
            state_->source->retiredProgram)
            return Reloaded::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto &source = *state_->source;
        auto projected = RuntimeUiPaintProjection::PrepareReload(preparation.generation, std::move(declaration), source.extraction);
        if (projected.HasError())
            return Reloaded::Failure(projected.ErrorValue());
        auto inventory = ResourceInventory(resources);
        if (inventory.HasError())
            return Reloaded::Failure(inventory.ErrorValue());
        auto arena = UiRenderGeometryArena::Create(source.geometryPolicy);
        if (arena.HasError())
            return Reloaded::Failure(arena.ErrorValue());
        const auto canvas = projected.Value()->Canvas();
        if (source.output.current) {
            const auto output = ResolveUiScreenCanvasWithEvidence(canvas, *source.output.current);
            if (output.HasError())
                return Reloaded::Failure(output.ErrorValue());
        }
        auto result = source.animation->Reload(state_->service, source.instance, std::move(preparation), cancellation);
        if (result.HasError())
            return result;
        // Nothing below can fail or acquire a source generation. The private projection already reserved/validated its storage.
        source.ReleaseFrame();
        source.retiredProgram = std::move(source.program);
        source.projection = std::move(projected).Value();
        source.arena = std::move(arena).Value();
        source.resources = std::move(resources);
        source.textures = std::move(inventory).Value();
        source.output.canvas = canvas;
        source.output.pending = source.output.current;
        source.output.awaitingPresentation = true;
        source.refresh = true;
        return result;
    }
}  // namespace Horo::Application::Internal
