#include "PrefabDocumentOverrideLayers.h"

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief Reject skewed layer metadata before the shared record validator can inspect or apply any intent. */
        Result<void> ValidateLayer(const std::optional<PrefabValueOverrideSet> &layer, Assets::AssetId source,
                                   const PrefabSourceRevision &revision, const PrefabLimitProfile &limits, std::size_t &bytes) {
            if (!layer)
                return Result<void>::Success();
            if (layer->sourcePrefab != source || layer->authoredAgainst != revision || layer->IsEmpty())
                return Result<void>::Failure(
                    MakeError(PrefabErrors::CompositionInvalid, "Override layer source/revision is inconsistent."));
            auto encoded = SerializePrefabValueOverrides(*layer, limits);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            if (!AddPayloadBytes(bytes, encoded.Value().size(), limits.Policy().maximumSourcePayloadBytes))
                return Result<void>::Failure(MakeError(PrefabErrors::PayloadTooLarge));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ParseOverrideLayer */
    Result<std::optional<PrefabValueOverrideSet>> ParseOverrideLayer(const Json &owner, const char *field,
                                                                     const PrefabProjectPolicy &policy) {
        if (!owner.contains(field))
            return Result<std::optional<PrefabValueOverrideSet>>::Success(std::nullopt);
        auto limits = PrefabLimitProfile::Create(policy);
        if (limits.HasError())
            return Result<std::optional<PrefabValueOverrideSet>>::Failure(limits.ErrorValue());
        auto parsed = ParsePrefabValueOverrides(owner.at(field).dump(0), limits.Value());
        if (parsed.HasError())
            return Result<std::optional<PrefabValueOverrideSet>>::Failure(parsed.ErrorValue());
        return Result<std::optional<PrefabValueOverrideSet>>::Success(std::move(parsed).Value());
    }

    /** @copydoc ValidateOverrideLayers */
    Result<void> ValidateOverrideLayers(const PrefabDocumentData &data, const PrefabLimitProfile &limits, std::size_t &payloadBytes) {
        if (!data.composition)
            return Result<void>::Success();
        const auto &composition = *data.composition;
        if (composition.variantParent)
            return ValidateLayer(composition.variantOverrides, composition.variantParent->Asset(), *composition.variantAuthoredAgainst,
                                 limits, payloadBytes);
        if (composition.variantOverrides)
            return Result<void>::Failure(MakeError(PrefabErrors::CompositionInvalid));
        for (const auto &placement : composition.nestedPlacements)
            if (auto valid = ValidateLayer(placement.valueOverrides, placement.sourcePrefab.Asset(), placement.authoredAgainst, limits,
                                           payloadBytes);
                valid.HasError())
                return valid;
        return Result<void>::Success();
    }

    /** @copydoc EncodeOverrideLayer */
    ValueDetail::Wire EncodeOverrideLayer(const PrefabValueOverrideSet &set) {
        return ValueDetail::EncodeSet(set, ValueDetail::EncodeCombinedRecords(set), ValueDetail::EncodeCombinedConflicts(set),
                                      ValueDetail::EncodeCombinedOrphans(set));
    }
}  // namespace Horo::Prefab::Detail
