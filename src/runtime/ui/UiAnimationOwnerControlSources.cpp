#include "UiAnimationOwnerInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Actual immutable arranged generation supplies source identity; no authored input DTO grants it. */
        UiActionOwnerContext Source(const UiLayoutSnapshot &layout) noexcept {
            const auto &descriptor = layout.Descriptor();
            return {descriptor.instance,         descriptor.canvas,       descriptor.document,
                    descriptor.sources.document, descriptor.sources.tree, descriptor.interaction};
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::PrepareControlSources */
    Result<void> UiAnimationOwner::PrepareControlSources(Storage &storage) {
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        const auto owner = Source(storage.candidate.layout->Candidate());
        auto &frame = *storage.frames[storage.candidate.frameSlot];
        frame.controls.clear();
        for (auto &element : canvas->controls) {
            if (auto prepared = element.control.PrepareInteractionReplacement(owner); prepared.HasError())
                return prepared;
            storage.candidate.controls.push_back(&element.control);
            frame.controls.push_back({element.id, {owner, element.control.Element()}, element.control.PreparedInteractionState()});
        }
        if (canvas->actions) {
            if (auto prepared = canvas->actions->PrepareInteractionReplacement(owner); prepared.HasError())
                return prepared;
            storage.candidate.canvasActions = &*canvas->actions;
        }
        if (canvas->bindings) {
            if (const auto writable = canvas->bindings->CanAdoptAnimationPresentation(canvas->tree, owner); writable.HasError())
                return Result<void>::Failure(writable.ErrorValue());
        }
        if (canvas->routes) {
            if (auto prepared = canvas->routes->PrepareActionInteractionReplacements(owner); prepared.HasError())
                return prepared;
            storage.candidate.actionSources = &*canvas->routes;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::CanPublishControlSources */
    Result<void> UiAnimationOwner::CanPublishControlSources(const Storage &storage) {
        const auto owner = Source(storage.candidate.layout->Candidate());
        for (const auto *control : storage.candidate.controls) {
            if (auto valid = control->CanPublishInteractionReplacement(owner); valid.HasError())
                return valid;
        }
        if (storage.candidate.canvasActions) {
            if (auto valid = storage.candidate.canvasActions->CanPublishInteractionReplacement(owner); valid.HasError())
                return valid;
        }
        const auto canvases = storage.candidate.source.Get()->Canvases();
        const auto canvas = std::ranges::find(canvases, storage.definition.canvas, &UiReloadCanvas::id);
        if (canvas == canvases.end())
            return Result<void>::Failure(MakeError(UiErrors::AnimationTargetStale));
        if (canvas->bindings) {
            if (const auto writable = canvas->bindings->CanAdoptAnimationPresentation(canvas->tree, owner); writable.HasError())
                return Result<void>::Failure(writable.ErrorValue());
        }
        return storage.candidate.actionSources ? storage.candidate.actionSources->CanPublishActionInteractionReplacements(owner)
                                               : Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::AbandonControlSources */
    void UiAnimationOwner::AbandonControlSources(Storage &storage) noexcept {
        for (auto *control : storage.candidate.controls)
            control->AbandonInteractionReplacement();
        storage.candidate.controls.clear();
        if (storage.candidate.actionSources)
            storage.candidate.actionSources->AbandonActionInteractionReplacements();
        storage.candidate.actionSources = nullptr;
        if (storage.candidate.canvasActions)
            storage.candidate.canvasActions->AbandonInteractionReplacement();
        storage.candidate.canvasActions = nullptr;
    }
}  // namespace Horo::Runtime::Ui
