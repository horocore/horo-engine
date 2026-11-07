#include "Horo/Foundation/Utf8.h"
#include "PrefabDocumentSerializationInternal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace Detail {
        /** @brief Validates parser bounds and an optional exact unified project-version
         * expectation. */
        [[nodiscard]] Result<void> ValidateParseLimits(const PrefabSourceParseLimits &limits) {
            if (limits.maximumSourceBytes == 0 || limits.maximumSourceBytes > PrefabHardLimits::SourceDocumentBytes ||
                limits.maximumJsonDepth == 0 || limits.maximumJsonDepth > PrefabHardLimits::SourceJsonDepth ||
                (limits.expectedProjectVersion && !IsCanonicalProjectVersion(*limits.expectedProjectVersion)))
                return Result<void>::Failure(Failure(PrefabErrors::DocumentInvalid, "Invalid prefab source parser limits."));
            return Result<void>::Success();
        }

        /** @brief Adds semantic payload bytes without allowing an intermediate
         * candidate to exceed policy. */
        [[nodiscard]] bool AddPayloadBytes(std::size_t &total, const std::size_t bytes, const std::size_t maximum) noexcept {
            if (bytes > maximum - total)
                return false;
            total += bytes;
            return true;
        }

        /** @brief Parses one opaque component envelope and accounts for its bounded
         * dynamic bytes. */
        [[nodiscard]] Result<RawComponentPayload> ParseComponent(const Json &value, std::size_t &payloadBytes,
                                                                 const std::size_t maximumPayloadBytes) {
            if (!HasAllowedFields(value, {"instanceId", "typeId", "schemaVersion", "encoding", "payload"}))
                return Failed<RawComponentPayload>(PrefabErrors::DocumentInvalid, "Prefab component fields are not canonical.");
            auto instance = ReadUnsigned<std::uint64_t>(value.at("instanceId"));
            if (instance.HasError())
                return Result<RawComponentPayload>::Failure(instance.ErrorValue());
            auto instanceId = PrefabComponentInstanceId::Create(instance.Value());
            if (instanceId.HasError())
                return Result<RawComponentPayload>::Failure(instanceId.ErrorValue());
            auto typeText = ReadString(value.at("typeId"));
            if (typeText.HasError())
                return Result<RawComponentPayload>::Failure(typeText.ErrorValue());
            auto typeId = Gameplay::ComponentTypeId::Parse(typeText.Value());
            if (typeId.HasError())
                return Failed<RawComponentPayload>(PrefabErrors::DocumentInvalid, "Prefab component type identity is invalid.");
            auto schemaVersion = ReadUnsigned<std::uint32_t>(value.at("schemaVersion"));
            if (schemaVersion.HasError() || schemaVersion.Value() == 0)
                return Failed<RawComponentPayload>(PrefabErrors::DocumentInvalid, "Prefab component schema version is invalid.");
            if (const auto encoding = ReadString(value.at("encoding")); encoding.HasError() || encoding.Value() != "canonicalJson")
                return Failed<RawComponentPayload>(PrefabErrors::UnsupportedPrefabSchema, "Prefab component encoding is unsupported.");
            auto bytes = ParseComponentBytes(value.at("payload"));
            if (bytes.HasError())
                return Result<RawComponentPayload>::Failure(bytes.ErrorValue());
            if (!AddPayloadBytes(payloadBytes, typeId.Value().Value().size(), maximumPayloadBytes) ||
                !AddPayloadBytes(payloadBytes, bytes.Value().size(), maximumPayloadBytes))
                return Failed<RawComponentPayload>(PrefabErrors::PayloadTooLarge, "Prefab component payload exceeds the document bound.");
            return Result<RawComponentPayload>::Success({.instance = instanceId.Value(),
                                                         .component = {.typeId = std::move(typeId).Value(),
                                                                       .schemaVersion = schemaVersion.Value(),
                                                                       .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                                       .payload = std::move(bytes).Value()}});
        }

        /** @brief Parses one object while bounding child payload counts before vector
         * allocation. */
        [[nodiscard]] Result<PrefabObjectNode> ParseObject(const Json &value, const PrefabProjectPolicy &policy,
                                                           std::size_t &payloadBytes) {
            if (!HasAllowedFields(value, {"localId", "parentLocalId", "name", "localTransform", "components", "behaviors"}))
                return Failed<PrefabObjectNode>(PrefabErrors::DocumentInvalid, "Prefab object fields are not canonical.");
            auto localId = ReadUnsigned<std::uint32_t>(value.at("localId"));
            if (localId.HasError())
                return Result<PrefabObjectNode>::Failure(localId.ErrorValue());
            auto parentId = ParseOptionalLocalObjectId(value.at("parentLocalId"));
            if (parentId.HasError())
                return Result<PrefabObjectNode>::Failure(parentId.ErrorValue());
            auto name = ReadString(value.at("name"));
            if (name.HasError())
                return Result<PrefabObjectNode>::Failure(name.ErrorValue());
            if (name.Value().size() > MaximumPrefabObjectNameBytes)
                return Failed<PrefabObjectNode>(PrefabErrors::DocumentInvalid, "Prefab object name exceeds its byte bound.");
            auto transform = ParseTransform(value.at("localTransform"));
            if (transform.HasError())
                return Result<PrefabObjectNode>::Failure(transform.ErrorValue());
            const Json &encodedComponents = value.at("components");
            const Json &encodedBehaviors = value.at("behaviors");
            if (!encodedComponents.is_array() || !encodedBehaviors.is_array() ||
                encodedComponents.size() > policy.maximumComponentsPerObject || encodedBehaviors.size() > policy.maximumComponentsPerObject)
                return Failed<PrefabObjectNode>(PrefabErrors::ComponentCountExceeded, "Prefab object component count exceeds its bound.");
            if (encodedComponents.size() > policy.maximumComponentsPerObject - encodedBehaviors.size())
                return Failed<PrefabObjectNode>(PrefabErrors::ComponentCountExceeded, "Prefab object component count exceeds its bound.");

            if (!AddPayloadBytes(payloadBytes, name.Value().size(), policy.maximumSourcePayloadBytes))
                return Failed<PrefabObjectNode>(PrefabErrors::PayloadTooLarge, "Prefab object names exceed the document bound.");
            std::vector<RawComponentPayload> components;
            components.reserve(encodedComponents.size());
            for (const Json &encodedComponent : encodedComponents) {
                auto component = ParseComponent(encodedComponent, payloadBytes, policy.maximumSourcePayloadBytes);
                if (component.HasError())
                    return Result<PrefabObjectNode>::Failure(component.ErrorValue());
                components.push_back(std::move(component).Value());
            }
            std::vector<Gameplay::BehaviorComponent> behaviors;
            behaviors.reserve(encodedBehaviors.size());
            for (const Json &encodedBehavior : encodedBehaviors) {
                auto behavior = ParseBehavior(encodedBehavior, payloadBytes, policy.maximumSourcePayloadBytes);
                if (behavior.HasError())
                    return Result<PrefabObjectNode>::Failure(behavior.ErrorValue());
                behaviors.push_back(std::move(behavior).Value());
            }
            return Result<PrefabObjectNode>::Success({.localId = {localId.Value()},
                                                      .parentLocalId = parentId.Value(),
                                                      .name = std::move(name).Value(),
                                                      .localTransform = transform.Value(),
                                                      .components = std::move(components),
                                                      .behaviors = std::move(behaviors)});
        }

        /** @brief Parses and accounts for the bounded Asset Registry dependency list.
         */
        [[nodiscard]] Result<std::vector<Assets::AssetId>> ParseReferences(const Json &value, const PrefabProjectPolicy &policy,
                                                                           std::size_t &payloadBytes) {
            if (!value.is_array() || value.size() > policy.maximumReferencedAssets)
                return Failed<std::vector<Assets::AssetId>>(PrefabErrors::ReferenceCountExceeded,
                                                            "Prefab reference count exceeds its bound.");
            std::vector<Assets::AssetId> references;
            references.reserve(value.size());
            for (const Json &encodedReference : value) {
                auto reference = ParseAssetId(encodedReference);
                if (reference.HasError())
                    return Result<std::vector<Assets::AssetId>>::Failure(reference.ErrorValue());
                if (!AddPayloadBytes(payloadBytes, reference.Value().Bytes().size(), policy.maximumSourcePayloadBytes))
                    return Failed<std::vector<Assets::AssetId>>(PrefabErrors::PayloadTooLarge,
                                                                "Prefab references exceed the document bound.");
                references.push_back(std::move(reference).Value());
            }
            return Result<std::vector<Assets::AssetId>>::Success(std::move(references));
        }

        /** @brief Parses and bounds the complete hierarchy array before semantic
         * validation. */
        [[nodiscard]] Result<std::vector<PrefabObjectNode>> ParseObjects(const Json &value, const PrefabProjectPolicy &policy,
                                                                         std::size_t &payloadBytes) {
            if (!value.is_array() || value.size() > policy.maximumObjectCount)
                return Failed<std::vector<PrefabObjectNode>>(PrefabErrors::ObjectCountExceeded, "Prefab object count exceeds its bound.");
            std::vector<PrefabObjectNode> objects;
            objects.reserve(value.size());
            for (const Json &encodedObject : value) {
                auto object = ParseObject(encodedObject, policy, payloadBytes);
                if (object.HasError())
                    return Result<std::vector<PrefabObjectNode>>::Failure(object.ErrorValue());
                objects.push_back(std::move(object).Value());
            }
            return Result<std::vector<PrefabObjectNode>>::Success(std::move(objects));
        }

        /** @brief Decodes a complete source DOM into one unpublished mutable candidate.
         */
        [[nodiscard]] Result<PrefabDocumentData> DecodeDocument(const Json &root, const PrefabLimitProfile &limits,
                                                                const PrefabSourceParseLimits &parseLimits) {
            if (!HasAllowedFields(root, {"projectVersion", "assetId", "objects", "referencedAssets"}, {"composition"}))
                return Failed<PrefabDocumentData>(PrefabErrors::DocumentInvalid, "Prefab document fields are not canonical.");
            auto projectVersion = ParseProjectVersion(root.at("projectVersion"));
            if (projectVersion.HasError())
                return Result<PrefabDocumentData>::Failure(projectVersion.ErrorValue());
            if (parseLimits.expectedProjectVersion && projectVersion.Value() != *parseLimits.expectedProjectVersion)
                return Failed<PrefabDocumentData>(PrefabErrors::UnsupportedPrefabSchema,
                                                  "Prefab projectVersion does not match the active project version.");
            auto assetId = ParseAssetId(root.at("assetId"));
            if (assetId.HasError())
                return Result<PrefabDocumentData>::Failure(assetId.ErrorValue());
            const PrefabProjectPolicy &policy = limits.Policy();
            std::size_t payloadBytes{};
            if (!AddPayloadBytes(payloadBytes, assetId.Value().Bytes().size(), policy.maximumSourcePayloadBytes))
                return Failed<PrefabDocumentData>(PrefabErrors::PayloadTooLarge, "Prefab identities exceed the document bound.");
            auto references = ParseReferences(root.at("referencedAssets"), policy, payloadBytes);
            if (references.HasError())
                return Result<PrefabDocumentData>::Failure(references.ErrorValue());
            auto objects = ParseObjects(root.at("objects"), policy, payloadBytes);
            if (objects.HasError())
                return Result<PrefabDocumentData>::Failure(objects.ErrorValue());

            std::optional<PrefabComposition> composition;
            if (root.contains("composition")) {
                auto parsedComposition = ParseComposition(root.at("composition"), policy);
                if (parsedComposition.HasError())
                    return Result<PrefabDocumentData>::Failure(parsedComposition.ErrorValue());
                composition = std::move(parsedComposition).Value();
            }
            return Result<PrefabDocumentData>::Success({.projectVersion = std::move(projectVersion).Value(),
                                                        .assetId = std::move(assetId).Value(),
                                                        .objects = std::move(objects).Value(),
                                                        .composition = std::move(composition),
                                                        .referencedAssets = std::move(references).Value()});
        }

        /** @brief Parses one bounded JSON DOM while retaining duplicate-key and depth
         * diagnostics. */
        [[nodiscard]] Result<Json> ParseJsonSource(const std::string_view source, const std::size_t maximumDepth) {
            Json root;
            try {
                {
                    JsonParseGuard guard{maximumDepth};
                    root = Json::parse(source, std::ref(guard), true, false);
                    if (guard.HasDuplicate())
                        return Failed<Json>(PrefabErrors::DocumentInvalid, "Prefab source contains a duplicate JSON field.");
                    if (guard.IsTooDeep())
                        return Failed<Json>(PrefabErrors::PayloadTooLarge, "Prefab source JSON depth exceeds its parser bound.");
                }
                if (root.is_discarded())
                    return Failed<Json>(PrefabErrors::DocumentInvalid, "Prefab source is not valid JSON.");
                return Result<Json>::Success(std::move(root));
            } catch (const Json::exception &) {
                return Failed<Json>(PrefabErrors::DocumentInvalid, "Prefab source is not valid JSON.");
            }
        }

        /** @brief Parses one complete source string and validates it before immutable
         * publication. */
        [[nodiscard]] Result<PrefabDocument> ParseDocumentImpl(const std::string_view source, const PrefabLimitProfile &limits,
                                                               const PrefabSourceParseLimits &parseLimits) {
            if (const auto validLimits = ValidateParseLimits(parseLimits); validLimits.HasError())
                return Result<PrefabDocument>::Failure(validLimits.ErrorValue());
            if (source.empty())
                return Failed<PrefabDocument>(PrefabErrors::DocumentInvalid, "Prefab source is empty.");
            if (source.size() > parseLimits.maximumSourceBytes)
                return Failed<PrefabDocument>(PrefabErrors::PayloadTooLarge, "Prefab source exceeds its parser byte bound.");
            if (!IsValidUtf8ScalarSequence(source))
                return Failed<PrefabDocument>(PrefabErrors::DocumentInvalid, "Prefab source is not valid UTF-8.");

            auto root = ParseJsonSource(source, parseLimits.maximumJsonDepth);
            if (root.HasError())
                return Result<PrefabDocument>::Failure(root.ErrorValue());
            auto decoded = DecodeDocument(root.Value(), limits, parseLimits);
            if (decoded.HasError())
                return Result<PrefabDocument>::Failure(decoded.ErrorValue());
            return PrefabDocument::Create(std::move(decoded).Value(), limits);
        }

    }  // namespace Detail

    /** @copydoc PrefabDocument::Parse */
    Result<PrefabDocument> PrefabDocument::Parse(const std::string_view source, const PrefabLimitProfile &limits,
                                                 const PrefabSourceParseLimits &parseLimits) {
        PrefabMetricOperation observation;
        if (parseLimits.metrics)
            observation = PrefabMetricOperation{*parseLimits.metrics, PrefabMetricPhase::Parse};
        observation.Observe(PrefabMetricItem::SourceBytes, source.size());
        auto result = Detail::ParseDocumentImpl(source, limits, parseLimits);
        if (result.HasError())
            observation.Fail(result.ErrorValue());
        else {
            observation.Observe(PrefabMetricItem::SourceObjects, result.Value().Data().objects.size());
            if (observation.Enabled()) {
                std::uint64_t components{};
                for (const auto &object : result.Value().Data().objects)
                    components += object.components.size() + object.behaviors.size();
                observation.Observe(PrefabMetricItem::Components, components);
            }
            observation.Complete();
        }
        return result;
    }

}  // namespace Horo::Prefab
