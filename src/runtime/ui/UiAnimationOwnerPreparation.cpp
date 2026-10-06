#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::Prepare */
    Result<UiAnimationOwner::Prepared> UiAnimationOwner::Prepare(const UiAnimationHostRead &read, const UiAnimationViewport &viewport) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped || storage_->draining ||
            storage_->candidate.admitted)
            return Result<Prepared>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto frame = std::ranges::find_if(storage_->frames, [&](const auto &slot) {
            const auto index = static_cast<std::uint32_t>(&slot - storage_->frames.data());
            return slot.use_count() == 1 && (!storage_->currentFrame || index != *storage_->currentFrame);
        });
        if (frame == storage_->frames.end())
            return Result<Prepared>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        auto source = storage_->publisher.Acquire();
        if (source.HasError())
            return Result<Prepared>::Failure(source.ErrorValue());
        auto &candidate = storage_->candidate;
        candidate.frameSlot = static_cast<std::uint32_t>(frame - storage_->frames.begin());
        candidate.commandRevision = storage_->commandRevision;
        candidate.sourceFrame = read.Frame();
        candidate.source = std::move(source).Value();
        candidate.admitted = true;
        Prepared reservation{storage_.PublisherPin(), candidate.frameSlot, candidate.commandRevision};
        auto &inactive = **frame;
        inactive.timelines.clear();
        inactive.markers.clear();
        if (const auto clocks = PrepareClocks(*storage_, read, inactive.clocks); clocks.HasError())
            return Result<Prepared>::Failure(clocks.ErrorValue());
        if (const auto child = PrepareRouteClock(*storage_); child.HasError())
            return Result<Prepared>::Failure(child.ErrorValue());
        if (const auto timelines = PrepareTimelines(*storage_); timelines.HasError())
            return Result<Prepared>::Failure(timelines.ErrorValue());
        if (const auto route = PrepareRouteProgress(*storage_); route.HasError())
            return Result<Prepared>::Failure(route.ErrorValue());
        if (const auto presentation = PreparePresentation(*storage_, viewport); presentation.HasError())
            return Result<Prepared>::Failure(presentation.ErrorValue());
        if (const auto valid = CanPublish(*storage_); valid.HasError())
            return Result<Prepared>::Failure(valid.ErrorValue());
        return Result<Prepared>::Success(std::move(reservation));
    }

    /** @copydoc UiAnimationOwner::PreparePresentation */
    Result<void> UiAnimationOwner::PreparePresentation(Storage &storage, const UiAnimationViewport &viewport) {
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        const auto &tree = canvas->tree;
        if (auto visual = PrepareVisualState(storage); visual.HasError())
            return visual;
        const UiStyleSourceRevisions lineage{tree.SourceDocumentRevision(),     tree.Revision(),
                                             storage.registry.Generation(),     storage.definition.content,
                                             storage.definition.resolvedPolicy, canvas->layoutEngine->PublishedInteraction()};
        auto style = storage.resolver.Prepare(tree, storage.registry, {lineage, storage.elementInputs});
        if (style.HasError())
            return Result<void>::Failure(style.ErrorValue());
        storage.candidate.styles.emplace(std::move(style).Value());
        for (std::size_t index = 0; index < storage.layoutDescriptors.size(); ++index)
            storage.layoutDescriptors[index] = {storage.elementInputs[index].element, storage.definition.elements[index].layout,
                                                storage.definition.elements[index].intrinsic};
        const auto &computed = storage.candidate.styles->Candidate();
        if (auto projected = AnimationInternal::ProjectLayout(computed, storage.layoutBindings, storage.layoutDescriptors);
            projected.HasError())
            return projected;
        // Layout lookup requires handle order; authored style inputs retain their separate tree-preorder contract.
        std::ranges::sort(storage.layoutDescriptors, {}, &UiLayoutElementDescriptor::element);
        auto evaluator = UiDeclarativeLayoutEvaluator::Create(storage.layoutDescriptors, storage.definition.intrinsicProvider.get());
        if (evaluator.HasError())
            return Result<void>::Failure(evaluator.ErrorValue());
        const UiLayoutSourceRevisions sources{lineage.document,
                                              lineage.tree,
                                              UiLayoutContentRevision::Create(lineage.content.Value()).Value(),
                                              UiLayoutStyleRevision::Create(computed.Descriptor().geometry.Value()).Value(),
                                              viewport.intrinsic,
                                              viewport.canvas,
                                              viewport.policy};
        auto layout =
            canvas->layoutEngine->Prepare(tree, {sources, viewport.constraints, viewport.content, &evaluator.Value(), viewport.fontScale});
        if (layout.HasError())
            return Result<void>::Failure(layout.ErrorValue());
        storage.candidate.layout.emplace(std::move(layout).Value());
        if (canvas->clipping) {
            auto clipped = canvas->clipping->Prepare(tree, storage.candidate.layout->Candidate(), {canvas->clipPolicies, {}});
            if (clipped.HasError())
                return Result<void>::Failure(clipped.ErrorValue());
            storage.candidate.clipping.emplace(std::move(clipped).Value());
        }
        if (auto focus = PrepareFocusGeometry(storage); focus.HasError())
            return focus;
        return PrepareControlSources(storage);
    }

    /** @copydoc UiAnimationOwner::CanPublish */
    Result<void> UiAnimationOwner::CanPublish(const Storage &storage) {
        const auto &candidate = storage.candidate;
        if (storage.stopped || storage.draining || !candidate.admitted || candidate.commandRevision != storage.commandRevision ||
            !storage.publisher.IsCurrent(candidate.source) || !candidate.styles || !candidate.layout)
            return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
        const auto canvases = candidate.source.Get()->Canvases();
        const auto canvas = std::ranges::find(canvases, storage.definition.canvas, &UiReloadCanvas::id);
        if (canvas == canvases.end())
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        if (const auto styles = candidate.styles->CanPublish(canvas->tree, storage.registry); styles.HasError())
            return styles;
        if (const auto layout = candidate.layout->CanPublish(canvas->tree); layout.HasError())
            return layout;
        if (canvas->clipping && !candidate.clipping)
            return Result<void>::Failure(MakeError(UiErrors::LayoutClipSourceStale));
        if (candidate.clipping) {
            if (auto clipping = candidate.clipping->CanPublish(canvas->tree); clipping.HasError())
                return clipping;
        }
        if (canvas->captures) {
            if (auto captures = canvas->captures->CanPublishInteraction(canvas->tree.Canvas()); captures.HasError())
                return captures;
        }
        if (const auto route = CanPublishRoute(storage); route.HasError())
            return route;
        return CanPublishControlSources(storage);
    }
}  // namespace Horo::Runtime::Ui
