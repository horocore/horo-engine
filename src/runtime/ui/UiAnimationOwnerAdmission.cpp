#include "UiAnimationAdmission.h"
#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::Create */
    Result<UiAnimationOwner> UiAnimationOwner::Create(UiReloadGeneration initial, UiElementSlotAllocator &allocator,
                                                      RuntimeStyleRegistry registry, UiStyleResolver styles,
                                                      UiAnimationCanvasDefinition definition, const UiAnimationLimits limits) {
        if (!AnimationInternal::ValidLimits(limits))
            return Result<UiAnimationOwner>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        const auto canvases = initial.Canvases();
        const auto canvas = std::ranges::find(canvases, definition.canvas, &UiReloadCanvas::id);
        if (canvas == canvases.end() || !canvas->tree.WasIssuedBy(allocator))
            return Result<UiAnimationOwner>::Failure(MakeError(UiErrors::AnimationTargetStale));
        // Source + six clocks + finite timeline slots share the actual tree issuer, and this complete range is burned on failure.
        auto reserved = allocator.Reserve(static_cast<std::uint32_t>(UiTimeDomainCount) + 1 + limits.timelines);
        if (reserved.HasError())
            return Result<UiAnimationOwner>::Failure(reserved.ErrorValue());
        auto publisher = UiHotReload::Create(std::move(initial));
        if (publisher.HasError())
            return Result<UiAnimationOwner>::Failure(publisher.ErrorValue());
        try {
            auto storage = std::make_shared<Storage>(std::move(publisher).Value(), std::move(registry), std::move(styles),
                                                     std::move(definition), limits, std::move(reserved).Value(), canvas->controls.size());
            if (const auto bound = InitializeBindings(*storage, *storage->publisher.Current()); bound.HasError())
                return Result<UiAnimationOwner>::Failure(bound.ErrorValue());
            return Result<UiAnimationOwner>::Success(UiAnimationOwner{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Result<UiAnimationOwner>::Failure(MakeError(UiErrors::AnimationStorageExhausted));
        }
    }

    /** @copydoc UiAnimationOwner::ReserveInteractionSources */
    Result<void> UiAnimationOwner::ReserveInteractionSources(UiReloadCanvas &canvas) {
        for (auto &element : canvas.controls) {
            if (auto reserved = element.control.ReserveInteractionReplacement(); reserved.HasError())
                return reserved;
        }
        if (canvas.actions) {
            if (auto reserved = canvas.actions->ReserveInteractionReplacement(); reserved.HasError())
                return reserved;
        }
        if (canvas.routes) {
            if (auto reserved = canvas.routes->ReserveActionInteractionReplacements(); reserved.HasError())
                return reserved;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::InitializeBindings */
    Result<void> UiAnimationOwner::InitializeBindings(Storage &storage, UiReloadGeneration &generation) {
        auto *canvas = generation.Canvas(storage.definition.canvas);
        if (const auto valid = AnimationInternal::ValidateDefinitions(storage.definition, *canvas, storage.registry, storage.limits);
            valid.HasError())
            return valid;
        if (auto reserved = ReserveInteractionSources(*canvas); reserved.HasError())
            return reserved;
        const auto &tree = canvas->tree;
        storage.source = {tree.Instance().ownership, storage.range.FirstSlot(), 1};
        for (std::size_t index = 0; index < UiTimeDomainCount; ++index) {
            auto &sample = storage.clocks.domains[index];
            sample.clock = {tree.Instance().ownership, storage.range.FirstSlot() + 1 + static_cast<std::uint32_t>(index), 1};
            sample.domain = static_cast<UiTimeDomain>(index);
        }
        for (std::size_t index = 0; index < storage.definition.elements.size(); ++index) {
            const auto &element = storage.definition.elements[index];
            const auto target = tree.Find(element.element);
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            storage.elementInputs[index] = {target.Value(),
                                            element.asset,
                                            element.typeClass,
                                            element.classes,
                                            element.inlineProperties,
                                            element.policyProperties,
                                            {},
                                            {}};
            storage.layoutDescriptors[index] = {target.Value(), element.layout, element.intrinsic};
        }
        for (const auto &binding : storage.definition.layoutBindings)
            storage.layoutBindings.push_back({tree.Find(binding.target).Value(), binding.property, binding.field});
        const UiStyleSourceRevisions sources{tree.SourceDocumentRevision(),     tree.Revision(),
                                             storage.registry.Generation(),     storage.definition.content,
                                             storage.definition.resolvedPolicy, canvas->layoutEngine->PublishedInteraction()};
        auto prepared = storage.resolver.Prepare(tree, storage.registry, {sources, storage.elementInputs});
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto reservation = std::move(prepared).Value();
        reservation.Abandon();
        // This resolver is a newly admitted geometry producer. A prior layout revision with the same numeric value
        // belongs to its previous composition and cannot qualify the first canonical projection.
        return canvas->layoutEngine->Invalidate({{}, tree.Revision(), UiLayoutDirtyKind::All});
    }
}  // namespace Horo::Runtime::Ui
