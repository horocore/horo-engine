#pragma once
#include "CanonicalJsonWriter.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabDocumentSerializationInternal.h"

namespace Horo::Prefab::ValueDetail {
    using Json = nlohmann::json;
    using Wire = JsonEncoding::Detail::CanonicalJsonValue<true>;

    template <class T> [[nodiscard]] Result<T> Invalid(std::string message = {}) {
        return Result<T>::Failure(MakeError(PrefabErrors::OverrideInvalid, std::move(message)));
    }

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
    [[nodiscard]] Result<PrefabOverrideValue> ReadPayloadValue(const Json &value, Gameplay::ComponentPropertyKind kind);
    [[nodiscard]] Json PayloadValue(const PrefabOverrideValue &value);
}  // namespace Horo::Prefab::ValueDetail
