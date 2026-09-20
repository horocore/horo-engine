#include "PrefabDocumentSerializationInternal.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Prefab::Detail {
    namespace {
        /** @brief Parses one vector-like behavior value while rejecting non-finite numbers and unknown kinds. */
        [[nodiscard]] Result<Gameplay::BehaviorFieldValue> ParseBehaviorVectorValue(const std::string_view kind, const Json &encoded) {
            if (kind == "vec2") {
                auto decoded = ParseFloatArray<2>(encoded);
                if (decoded.HasError())
                    return Result<Gameplay::BehaviorFieldValue>::Failure(decoded.ErrorValue());
                return Result<Gameplay::BehaviorFieldValue>::Success(Math::Vec2{decoded.Value()[0], decoded.Value()[1]});
            }
            if (kind == "vec3") {
                auto decoded = ParseFloatArray<3>(encoded);
                if (decoded.HasError())
                    return Result<Gameplay::BehaviorFieldValue>::Failure(decoded.ErrorValue());
                return Result<Gameplay::BehaviorFieldValue>::Success(
                    Math::Vec3{decoded.Value()[0], decoded.Value()[1], decoded.Value()[2]});
            }
            if (kind == "quaternion") {
                auto decoded = ParseFloatArray<4>(encoded);
                if (decoded.HasError())
                    return Result<Gameplay::BehaviorFieldValue>::Failure(decoded.ErrorValue());
                return Result<Gameplay::BehaviorFieldValue>::Success(
                    Math::Quaternion{decoded.Value()[0], decoded.Value()[1], decoded.Value()[2], decoded.Value()[3]});
            }
            return Failed<Gameplay::BehaviorFieldValue>(PrefabErrors::UnsupportedPrefabSchema,
                                                        "Prefab behavior value kind is unsupported.");
        }

        /** @brief Parses one scalar behavior value while rejecting non-finite numbers and unknown kinds. */
        [[nodiscard]] Result<Gameplay::BehaviorFieldValue> ParseBehaviorValue(const Json &value) {
            if (!HasAllowedFields(value, {"type", "value"}))
                return Failed<Gameplay::BehaviorFieldValue>(PrefabErrors::DocumentInvalid,
                                                            "Prefab behavior value fields are not canonical.");
            auto kind = ReadString(value.at("type"));
            if (kind.HasError())
                return Result<Gameplay::BehaviorFieldValue>::Failure(kind.ErrorValue());
            const Json &encoded = value.at("value");
            if (kind.Value() == "null") {
                if (!encoded.is_null())
                    return Failed<Gameplay::BehaviorFieldValue>(PrefabErrors::DocumentInvalid, "Prefab null behavior value is malformed.");
                return Result<Gameplay::BehaviorFieldValue>::Success(std::monostate{});
            }
            if (kind.Value() == "bool") {
                if (!encoded.is_boolean())
                    return Failed<Gameplay::BehaviorFieldValue>(PrefabErrors::DocumentInvalid,
                                                                "Prefab boolean behavior value is malformed.");
                return Result<Gameplay::BehaviorFieldValue>::Success(encoded.get<bool>());
            }
            if (kind.Value() == "int") {
                if (encoded.is_number_integer())
                    return Result<Gameplay::BehaviorFieldValue>::Success(encoded.get<std::int64_t>());
                if (encoded.is_number_unsigned() && encoded.get<std::uint64_t>() <= std::numeric_limits<std::int64_t>::max())
                    return Result<Gameplay::BehaviorFieldValue>::Success(static_cast<std::int64_t>(encoded.get<std::uint64_t>()));
                return Failed<Gameplay::BehaviorFieldValue>(PrefabErrors::DocumentInvalid, "Prefab integer behavior value is malformed.");
            }
            if (kind.Value() == "number") {
                auto number = ReadDouble(encoded);
                if (number.HasError())
                    return Result<Gameplay::BehaviorFieldValue>::Failure(number.ErrorValue());
                return Result<Gameplay::BehaviorFieldValue>::Success(number.Value());
            }
            if (kind.Value() == "string") {
                auto text = ReadString(encoded);
                if (text.HasError())
                    return Result<Gameplay::BehaviorFieldValue>::Failure(text.ErrorValue());
                return Result<Gameplay::BehaviorFieldValue>::Success(std::move(text).Value());
            }
            return ParseBehaviorVectorValue(kind.Value(), encoded);
        }

        /** @brief Parses one bounded behavior field and accounts for its dynamic text bytes. */
        [[nodiscard]] Result<Gameplay::BehaviorField> ParseBehaviorField(const Json &value, std::size_t &payloadBytes,
                                                                         const std::size_t maximumPayloadBytes) {
            if (!HasAllowedFields(value, {"name", "value"}))
                return Failed<Gameplay::BehaviorField>(PrefabErrors::DocumentInvalid, "Prefab behavior field is not canonical.");
            auto name = ReadString(value.at("name"));
            if (name.HasError())
                return Result<Gameplay::BehaviorField>::Failure(name.ErrorValue());
            auto fieldValue = ParseBehaviorValue(value.at("value"));
            if (fieldValue.HasError())
                return Result<Gameplay::BehaviorField>::Failure(fieldValue.ErrorValue());
            if (const auto *text = std::get_if<std::string>(&fieldValue.Value());
                !AddPayloadBytes(payloadBytes, name.Value().size(), maximumPayloadBytes) ||
                !AddPayloadBytes(payloadBytes, text == nullptr ? 0 : text->size(), maximumPayloadBytes))
                return Failed<Gameplay::BehaviorField>(PrefabErrors::PayloadTooLarge, "Prefab behavior fields exceed the document bound.");
            return Result<Gameplay::BehaviorField>::Success({.name = std::move(name).Value(), .value = std::move(fieldValue).Value()});
        }

        /** @brief Parses one concrete nested placement record. */
        [[nodiscard]] Result<NestedPrefabPlacement> ParseNestedPlacement(const Json &value) {
            if (!HasAllowedFields(value, {"placementLocalId", "parentLocalId", "sourceAsset", "authoredAgainst", "localRootTransform"}))
                return Failed<NestedPrefabPlacement>(PrefabErrors::InvalidPlacement, "Prefab nested placement fields are not canonical.");
            auto placementId = ReadUnsigned<std::uint32_t>(value.at("placementLocalId"));
            if (placementId.HasError())
                return Result<NestedPrefabPlacement>::Failure(placementId.ErrorValue());
            auto parentId = ParseOptionalLocalObjectId(value.at("parentLocalId"));
            if (parentId.HasError())
                return Result<NestedPrefabPlacement>::Failure(parentId.ErrorValue());
            auto source = ParseAssetId(value.at("sourceAsset"));
            if (source.HasError())
                return Result<NestedPrefabPlacement>::Failure(source.ErrorValue());
            auto revision = ParseRevision(value.at("authoredAgainst"));
            if (revision.HasError())
                return Result<NestedPrefabPlacement>::Failure(revision.ErrorValue());
            auto transform = ParseTransform(value.at("localRootTransform"));
            if (transform.HasError())
                return Result<NestedPrefabPlacement>::Failure(transform.ErrorValue());
            auto reference = PrefabAssetReference::Create(source.Value());
            if (reference.HasError())
                return Result<NestedPrefabPlacement>::Failure(reference.ErrorValue());
            return Result<NestedPrefabPlacement>::Success({.placementLocalId = {placementId.Value()},
                                                           .parentLocalId = parentId.Value(),
                                                           .sourcePrefab = reference.Value(),
                                                           .authoredAgainst = revision.Value(),
                                                           .localRootTransform = transform.Value()});
        }

        /** @brief Parses the exclusive variant composition envelope. */
        [[nodiscard]] Result<std::optional<PrefabComposition>> ParseVariantComposition(const Json &value) {
            if (!HasAllowedFields(value, {"variantParent", "variantAuthoredAgainst"}))
                return Failed<std::optional<PrefabComposition>>(PrefabErrors::CompositionInvalid,
                                                                "Prefab variant composition fields are not canonical.");
            auto parent = ParseAssetId(value.at("variantParent"));
            if (parent.HasError())
                return Result<std::optional<PrefabComposition>>::Failure(parent.ErrorValue());
            auto revision = ParseRevision(value.at("variantAuthoredAgainst"));
            if (revision.HasError())
                return Result<std::optional<PrefabComposition>>::Failure(revision.ErrorValue());
            auto reference = PrefabAssetReference::Create(parent.Value());
            if (reference.HasError())
                return Result<std::optional<PrefabComposition>>::Failure(reference.ErrorValue());
            return Result<std::optional<PrefabComposition>>::Success(
                PrefabComposition{.variantParent = reference.Value(), .variantAuthoredAgainst = revision.Value()});
        }

        /** @brief Parses the bounded concrete nested-placement envelope. */
        [[nodiscard]] Result<std::optional<PrefabComposition>> ParseNestedComposition(const Json &value,
                                                                                      const PrefabProjectPolicy &policy) {
            if (!HasAllowedFields(value, {"nestedPlacements"}))
                return Failed<std::optional<PrefabComposition>>(PrefabErrors::CompositionInvalid,
                                                                "Prefab concrete composition fields are not canonical.");
            const Json &encodedPlacements = value.at("nestedPlacements");
            if (!encodedPlacements.is_array() || encodedPlacements.size() > policy.maximumDirectNestedPlacements)
                return Failed<std::optional<PrefabComposition>>(PrefabErrors::NestedPlacementCountExceeded,
                                                                "Prefab nested placement count exceeds its bound.");
            std::vector<NestedPrefabPlacement> placements;
            placements.reserve(encodedPlacements.size());
            for (const Json &encodedPlacement : encodedPlacements) {
                auto placement = ParseNestedPlacement(encodedPlacement);
                if (placement.HasError())
                    return Result<std::optional<PrefabComposition>>::Failure(placement.ErrorValue());
                placements.push_back(std::move(placement).Value());
            }
            return Result<std::optional<PrefabComposition>>::Success(PrefabComposition{.nestedPlacements = std::move(placements)});
        }
    }  // namespace

    /** @brief Parses one behavior component with bounded field count and stable typed identity. */
    [[nodiscard]] Result<Gameplay::BehaviorComponent> ParseBehavior(const Json &value, std::size_t &payloadBytes,
                                                                    const std::size_t maximumPayloadBytes) {
        if (!HasAllowedFields(value, {"instanceId", "typeId", "schemaVersion", "enabled", "fields"}))
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::DocumentInvalid, "Prefab behavior fields are not canonical.");
        auto instance = ReadUnsigned<std::uint64_t>(value.at("instanceId"));
        if (instance.HasError() || instance.Value() == 0)
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::IdentityInvalid, "Prefab behavior identity is invalid.");
        auto typeText = ReadString(value.at("typeId"));
        if (typeText.HasError())
            return Result<Gameplay::BehaviorComponent>::Failure(typeText.ErrorValue());
        auto typeId = Gameplay::BehaviorTypeId::Parse(typeText.Value());
        if (typeId.HasError())
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::DocumentInvalid, "Prefab behavior type identity is invalid.");
        auto schemaVersion = ReadUnsigned<std::uint32_t>(value.at("schemaVersion"));
        if (schemaVersion.HasError() || schemaVersion.Value() == 0 || !value.at("enabled").is_boolean())
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::DocumentInvalid,
                                                       "Prefab behavior schema or enabled state is invalid.");
        const Json &encodedFields = value.at("fields");
        if (!encodedFields.is_array() || encodedFields.size() > Gameplay::MaximumBehaviorFields)
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::ComponentCountExceeded,
                                                       "Prefab behavior field count exceeds its bound.");
        std::vector<Gameplay::BehaviorField> fields;
        fields.reserve(encodedFields.size());
        for (const Json &encodedField : encodedFields) {
            auto field = ParseBehaviorField(encodedField, payloadBytes, maximumPayloadBytes);
            if (field.HasError())
                return Result<Gameplay::BehaviorComponent>::Failure(field.ErrorValue());
            fields.push_back(std::move(field).Value());
        }
        if (!AddPayloadBytes(payloadBytes, typeId.Value().Value().size(), maximumPayloadBytes))
            return Failed<Gameplay::BehaviorComponent>(PrefabErrors::PayloadTooLarge, "Prefab behavior types exceed the document bound.");
        return Result<Gameplay::BehaviorComponent>::Success({.instanceId = {instance.Value()},
                                                             .typeId = std::move(typeId).Value(),
                                                             .schemaVersion = schemaVersion.Value(),
                                                             .enabled = value.at("enabled").get<bool>(),
                                                             .fields = std::move(fields)});
    }

    /** @brief Parses a concrete nested placement or exclusive variant composition envelope. */
    [[nodiscard]] Result<std::optional<PrefabComposition>> ParseComposition(const Json &value, const PrefabProjectPolicy &policy) {
        if (value.is_null() || !value.is_object())
            return Failed<std::optional<PrefabComposition>>(PrefabErrors::CompositionInvalid,
                                                            "Prefab composition must be an object when present.");
        return value.contains("variantParent") ? ParseVariantComposition(value) : ParseNestedComposition(value, policy);
    }
}  // namespace Horo::Prefab::Detail
