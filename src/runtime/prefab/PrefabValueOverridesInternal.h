#pragma once
#include "CanonicalJsonWriter.h"
#include "Horo/Prefab/PrefabOverrideOperations.h"
#include "PrefabValueJsonInternal.h"

namespace Horo::Prefab::ValueDetail {
    using OwnedJson = JsonEncoding::Detail::OwnedJsonValue;

    using Json = nlohmann::json;
    using Wire = JsonEncoding::Detail::CanonicalJsonValue<true>;

    template <class T> [[nodiscard]] Result<T> Invalid(std::string message = {}) {
        return Result<T>::Failure(MakeError(PrefabErrors::OverrideInvalid, std::move(message)));
    }

    /** @brief Composes actual placement/local matrices only when the final affine
     * result is representable as TRS within engine precision.
     */
    [[nodiscard]] Result<Math::Transform> ComposePrefabPlacement(const Math::Transform &placement, const Math::Transform &local);
    [[nodiscard]] Result<void> ValidateValue(const PrefabOverrideValue &value, const PrefabValuePropertySchema *schema = nullptr);
    [[nodiscard]] Wire EncodeValue(const PrefabOverrideValue &value);
    [[nodiscard]] Result<PrefabOverrideValue> DecodeValue(const Json &value);
    [[nodiscard]] bool TargetLess(const PrefabValueOverrideTarget &left, const PrefabValueOverrideTarget &right) noexcept;
    [[nodiscard]] Wire EncodeTarget(const PrefabValueOverrideTarget &target);
    [[nodiscard]] Result<PrefabValueOverrideTarget> DecodeTarget(const Json &value);
    [[nodiscard]] Wire EncodeRevision(const PrefabSourceRevision &revision);
    [[nodiscard]] Wire EncodeRecord(const PrefabValueOverrideRecord &record);
    [[nodiscard]] Result<PrefabValueOverrideRecord> DecodeRecord(const Json &value);
    [[nodiscard]] Sha256Digest ValueDigest(const PrefabOverrideValue &value);
    [[nodiscard]] bool SameValue(const PrefabOverrideValue &left, const PrefabOverrideValue &right);
    /** @brief Parses only bounded unique-key component objects into
     * allocation-free-retiring ownership, preserving caller diagnostics. */
    [[nodiscard]] Result<OwnedJson> DecodeComponentPayload(std::span<const std::byte> bytes, std::string_view failure);
    [[nodiscard]] Result<PrefabOverrideValue> ReadPayloadValue(const Json &value, Gameplay::ComponentPropertyKind kind);
    [[nodiscard]] OwnedJson PayloadValue(const PrefabOverrideValue &value);
    [[nodiscard]] std::strong_ordering CompareObjectAddresses(const PrefabObjectAddress &left, const PrefabObjectAddress &right) noexcept;
    [[nodiscard]] Result<PrefabObjectAddress> DecodeObjectAddress(const Json &value);
    [[nodiscard]] PrefabComponentOverrideTarget ComponentTarget(const PrefabComponentOverrideRecord &record);
    [[nodiscard]] Wire EncodeComponentRecord(const PrefabComponentOverrideRecord &record);
    [[nodiscard]] Result<PrefabComponentOverrideRecord> DecodeComponentRecord(const Json &value);
    [[nodiscard]] bool ComponentRecordLess(const PrefabComponentOverrideRecord &left, const PrefabComponentOverrideRecord &right);
    [[nodiscard]] bool IsComponentRecord(const Json &value);
    [[nodiscard]] Sha256Digest ComponentDigest(const RawComponentPayload &component);
    [[nodiscard]] Wire EncodeValueConflict(const PrefabValueOverrideConflict &conflict);
    [[nodiscard]] Wire EncodeValueOrphan(const PrefabValueOverrideOrphan &orphan);
    [[nodiscard]] Wire EncodeComponentConflict(const PrefabComponentOverrideConflict &conflict);
    [[nodiscard]] Wire EncodeComponentOrphan(const PrefabComponentOverrideOrphan &orphan);
    [[nodiscard]] Result<PrefabComponentOverrideConflict> DecodeComponentConflict(const Json &value);
    [[nodiscard]] Result<PrefabComponentOverrideOrphan> DecodeComponentOrphan(const Json &value);
    /** @brief Encode the single override envelope for standalone and
     * composition-layer serialization. */
    [[nodiscard]] Wire EncodeSet(const PrefabValueOverrideSet &set, Wire records = Wire::array(), Wire conflicts = Wire::array(),
                                 Wire orphans = Wire::array());
    [[nodiscard]] Wire EncodeCombinedRecords(const PrefabValueOverrideSet &set);
    [[nodiscard]] Wire EncodeCombinedConflicts(const PrefabValueOverrideSet &set);
    [[nodiscard]] Wire EncodeCombinedOrphans(const PrefabValueOverrideSet &set);
    [[nodiscard]] Result<void> ValidateOverrideCounts(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy);
    [[nodiscard]] Result<void> PreflightComponentClaims(const PrefabValueOverrideSet &set);
    [[nodiscard]] Result<void> ValidateComponentRecords(const PrefabValueOverrideSet &set, const PrefabProjectPolicy &policy,
                                                        std::size_t &bytes);
    [[nodiscard]] Result<Sha256Digest> CaptureComponentSchemas(std::vector<PrefabComponentOverridePolicy> &components,
                                                               std::vector<PrefabBehaviorComponentPolicy> &behaviors,
                                                               std::span<const PrefabValuePropertySchema> properties,
                                                               const Sha256Digest &propertyRevision, const PrefabLimitProfile &limits);
}  // namespace Horo::Prefab::ValueDetail
