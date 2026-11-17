#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabDocumentSession.h"

#include <algorithm>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    /** @copydoc PrefabDocumentSession::Layer */
    Result<PrefabDocumentSession::LayerReplacement> PrefabDocumentSession::Layer(
        const std::optional<Prefab::LocalObjectId> placement) const {
        using Outcome = Result<LayerReplacement>;
        const auto &composition = source_->Data().composition;
        if (!composition)
            return Outcome::Failure(MakeError(Prefab::PrefabErrors::CompositionInvalid));
        if (!placement) {
            if (!composition->variantParent || !composition->variantAuthoredAgainst)
                return Outcome::Failure(MakeError(Prefab::PrefabErrors::CompositionInvalid));
            return Outcome::Success({{}, *composition->variantAuthoredAgainst, composition->variantOverrides});
        }
        const auto found = std::ranges::find(composition->nestedPlacements, *placement, &Prefab::NestedPrefabPlacement::placementLocalId);
        if (found == composition->nestedPlacements.end())
            return Outcome::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
        return Outcome::Success({placement, found->authoredAgainst, found->valueOverrides});
    }

    /** @copydoc PrefabDocumentSession::Before */
    Result<PrefabDocumentSession::Edit> PrefabDocumentSession::Before(const Edit &edit) const {
        return std::visit([&]<typename T>(const T &value) -> Result<Edit> {
            if constexpr (std::is_same_v<T, LayerReplacement>) {
                auto layer = Layer(value.placement);
                return layer.HasError() ? Result<Edit>::Failure(layer.ErrorValue()) : Result<Edit>::Success(Edit{std::move(layer).Value()});
            } else {
                const auto found = std::ranges::find(source_->Data().objects, value.object, &Prefab::PrefabObjectNode::localId);
                if (found == source_->Data().objects.end())
                    return Result<Edit>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
                if constexpr (std::is_same_v<T, RenamePrefabObject>)
                    return Result<Edit>::Success(Edit{RenamePrefabObject{value.object, found->name}});
                else
                    return Result<Edit>::Success(Edit{TransformPrefabObject{value.object, found->localTransform}});
            }
        }, edit);
    }

    /** @copydoc PrefabDocumentSession::EditBytes */
    Result<std::size_t> PrefabDocumentSession::EditBytes(const Edit &before, const Edit &after) const {
        std::size_t bytes = sizeof(Delta);
        for (const auto *edit : {&before, &after}) {
            auto measured = std::visit([&]<typename T>(const T &value) -> Result<std::size_t> {
                if constexpr (std::is_same_v<T, RenamePrefabObject>)
                    return Result<std::size_t>::Success(value.name.size());
                else if constexpr (std::is_same_v<T, LayerReplacement>) {
                    if (!value.overrides)
                        return Result<std::size_t>::Success(0);
                    auto encoded = Prefab::SerializePrefabValueOverrides(*value.overrides, limits_);
                    return encoded.HasError() ? Result<std::size_t>::Failure(encoded.ErrorValue())
                                              : Result<std::size_t>::Success(encoded.Value().size());
                } else
                    return Result<std::size_t>::Success(0);
            }, *edit);
            if (measured.HasError())
                return measured;
            bytes += measured.Value();  // Each side is already bounded by the canonical source-byte limit.
        }
        return Result<std::size_t>::Success(bytes);
    }

    /** @copydoc PrefabDocumentSession::ExecuteLayer */
    Result<void> PrefabDocumentSession::ExecuteLayer(const PrefabDocumentFence &fence, const std::optional<Prefab::LocalObjectId> placement,
                                                     const Prefab::PrefabSourceResolverSnapshot &sources,
                                                     const std::variant<Prefab::PrefabValueEdit, Prefab::PrefabComponentEdit> &edit,
                                                     const CancellationToken &cancellation) {
        try {
            return ExecuteLayerImpl(fence, placement, sources, edit, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PrefabDocumentSession::ExecuteLayerImpl */
    Result<void> PrefabDocumentSession::ExecuteLayerImpl(const PrefabDocumentFence &fence,
                                                         const std::optional<Prefab::LocalObjectId> placement,
                                                         const Prefab::PrefabSourceResolverSnapshot &sources,
                                                         const std::variant<Prefab::PrefabValueEdit, Prefab::PrefabComponentEdit> &edit,
                                                         const CancellationToken &cancellation) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        auto layer = Layer(placement);
        if (layer.HasError())
            return Result<void>::Failure(layer.ErrorValue());
        const auto &composition = *source_->Data().composition;
        const auto asset =
            placement ? std::ranges::find(composition.nestedPlacements, *placement, &Prefab::NestedPrefabPlacement::placementLocalId)
                            ->sourcePrefab.Asset()
                      : composition.variantParent->Asset();
        auto source = sources.Resolve(asset, Prefab::PrefabInstanceId::Create(identity_.instance.Value()).Value(), limits_, cancellation);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        auto prepared = std::visit([&]<typename T>(const T &intent) {
            if constexpr (std::is_same_v<T, Prefab::PrefabValueEdit>)
                return Prefab::PreparePrefabValueEdit(source.Value(), layer.Value().overrides, intent, sources.ValueSchemas(), limits_,
                                                      cancellation);
            else
                return Prefab::PreparePrefabComponentEdit(source.Value(), layer.Value().overrides, intent, sources.ValueSchemas(), limits_,
                                                          cancellation);
        }, edit);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
        auto rebased = std::move(prepared).Value().rebased;
        LayerReplacement replacement{placement, source.Value().Revision().rootSource, {}};
        if (!rebased.IsEmpty())
            replacement.overrides = std::move(rebased);
        return CommitEdit(Edit{std::move(replacement)});
    }
}  // namespace Horo::Editor
