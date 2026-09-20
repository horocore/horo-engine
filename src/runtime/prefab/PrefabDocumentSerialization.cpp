#include "Horo/Foundation/Utf8.h"
#include "PrefabDocumentSerializationInternal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Horo::Prefab {
    namespace Detail {
        /** @brief Reports whether an object contains exactly required fields plus an optional field set. */
        [[nodiscard]] bool HasAllowedFields(const Json &value, const std::initializer_list<std::string_view> required,
                                            const std::initializer_list<std::string_view> optional) {
            if (!value.is_object() || value.size() < required.size() || value.size() > required.size() + optional.size())
                return false;
            for (const std::string_view field : required) {
                if (!value.contains(std::string{field}))
                    return false;
            }
            return std::ranges::all_of(value.items(), [&](const auto &entry) {
                const std::string_view name = entry.key();
                return std::ranges::find(required, name) != required.end() || std::ranges::find(optional, name) != optional.end();
            });
        }

        /** @brief Rejects duplicate object keys and excessive nesting before the JSON DOM is admitted. */
        class JsonParseGuard final {
        public:
            explicit JsonParseGuard(const std::size_t maximumDepth) : maximumDepth_(maximumDepth) {}

            bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || static_cast<std::size_t>(depth) >= maximumDepth_) {
                    tooDeep_ = true;
                    return false;
                }
                const auto index =
                    event == Json::parse_event_t::key && depth > 0 ? static_cast<std::size_t>(depth - 1) : static_cast<std::size_t>(depth);
                if (keys_.size() <= index)
                    keys_.resize(index + 1);
                if (event == Json::parse_event_t::object_start)
                    keys_[index].clear();
                if (event == Json::parse_event_t::key && !keys_[index].insert(value.get<std::string>()).second)
                    duplicate_ = true;
                if (event == Json::parse_event_t::object_end)
                    keys_[index].clear();
                return !duplicate_ && !tooDeep_;
            }

            [[nodiscard]] bool HasDuplicate() const noexcept {
                return duplicate_;
            }

            [[nodiscard]] bool IsTooDeep() const noexcept {
                return tooDeep_;
            }

        private:
            std::size_t maximumDepth_{};
            std::vector<std::unordered_set<std::string>> keys_;
            bool duplicate_{};
            bool tooDeep_{};
        };

        /** @brief Validates parser bounds and an optional exact unified project-version expectation. */
        [[nodiscard]] Result<void> ValidateParseLimits(const PrefabSourceParseLimits &limits) {
            if (limits.maximumSourceBytes == 0 || limits.maximumSourceBytes > PrefabHardLimits::SourceDocumentBytes ||
                limits.maximumJsonDepth == 0 || limits.maximumJsonDepth > PrefabHardLimits::SourceJsonDepth ||
                (limits.expectedProjectVersion && !IsCanonicalProjectVersion(*limits.expectedProjectVersion)))
                return Result<void>::Failure(Failure(PrefabErrors::DocumentInvalid, "Invalid prefab source parser limits."));
            return Result<void>::Success();
        }

        /** @brief Parses one UTF-8 JSON string while retaining only scalar-valid text. */
        [[nodiscard]] Result<std::string> ReadString(const Json &value) {
            if (!value.is_string())
                return Failed<std::string>(PrefabErrors::DocumentInvalid, "Prefab source field must be a string.");
            std::string decoded = value.get<std::string>();
            if (!IsValidUtf8ScalarSequence(decoded))
                return Failed<std::string>(PrefabErrors::DocumentInvalid, "Prefab source contains invalid UTF-8 text.");
            return Result<std::string>::Success(std::move(decoded));
        }

        /** @brief Parses one finite single-precision JSON number without accepting overflow. */
        [[nodiscard]] Result<float> ReadFloat(const Json &value) {
            if (!value.is_number())
                return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is not numeric.");
            const double decoded = value.get<double>();
            if (!std::isfinite(decoded) || std::abs(decoded) > std::numeric_limits<float>::max())
                return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is non-finite or out of range.");
            const auto converted = static_cast<float>(decoded);
            if (!std::isfinite(converted))
                return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is non-finite.");
            return Result<float>::Success(converted);
        }

        /** @brief Parses one finite double JSON number. */
        [[nodiscard]] Result<double> ReadDouble(const Json &value) {
            if (!value.is_number())
                return Failed<double>(PrefabErrors::DocumentInvalid, "Prefab behavior value is not numeric.");
            const double decoded = value.get<double>();
            if (!std::isfinite(decoded))
                return Failed<double>(PrefabErrors::DocumentInvalid, "Prefab behavior value is non-finite.");
            return Result<double>::Success(decoded);
        }

        /** @brief Parses a canonical Asset Registry identity and rejects path-shaped references. */
        [[nodiscard]] Result<Assets::AssetId> ParseAssetId(const Json &value) {
            auto text = ReadString(value);
            if (text.HasError())
                return Result<Assets::AssetId>::Failure(text.ErrorValue());
            auto parsed = Assets::AssetId::Parse(text.Value());
            if (parsed.HasError() || parsed.Value().ToString() != text.Value())
                return Failed<Assets::AssetId>(PrefabErrors::ReferenceInvalid, "Prefab references must use canonical AssetId text.");
            return parsed;
        }

        /** @brief Parses the one unified project version used by all durable project assets. */
        [[nodiscard]] Result<Application::HoroVersion> ParseProjectVersion(const Json &value) {
            auto text = ReadString(value);
            if (text.HasError())
                return Result<Application::HoroVersion>::Failure(text.ErrorValue());
            auto parsed = Application::ParseHoroVersion(text.Value());
            if (parsed.HasError() || Application::FormatHoroVersion(parsed.Value()) != text.Value())
                return Failed<Application::HoroVersion>(PrefabErrors::UnsupportedPrefabSchema,
                                                        "Prefab projectVersion is not canonical or supported.");
            return parsed;
        }

        /** @brief Parses one optional root-inclusive local object identity. */
        [[nodiscard]] Result<std::optional<LocalObjectId>> ParseOptionalLocalObjectId(const Json &value) {
            if (value.is_null())
                return Result<std::optional<LocalObjectId>>::Success(std::nullopt);
            auto parsed = ReadUnsigned<std::uint32_t>(value);
            if (parsed.HasError())
                return Result<std::optional<LocalObjectId>>::Failure(parsed.ErrorValue());
            return Result<std::optional<LocalObjectId>>::Success(LocalObjectId{parsed.Value()});
        }

        /** @brief Parses the backend-neutral transform object used by every hierarchy record. */
        [[nodiscard]] Result<Math::Transform> ParseTransform(const Json &value) {
            if (!HasAllowedFields(value, {"translation", "rotation", "scale"}))
                return Failed<Math::Transform>(PrefabErrors::DocumentInvalid, "Prefab transform fields are not canonical.");
            auto translation = ParseFloatArray<3>(value.at("translation"));
            auto rotation = ParseFloatArray<4>(value.at("rotation"));
            auto scale = ParseFloatArray<3>(value.at("scale"));
            if (translation.HasError())
                return Result<Math::Transform>::Failure(translation.ErrorValue());
            if (rotation.HasError())
                return Result<Math::Transform>::Failure(rotation.ErrorValue());
            if (scale.HasError())
                return Result<Math::Transform>::Failure(scale.ErrorValue());
            return Result<Math::Transform>::Success(
                {.translation = {translation.Value()[0], translation.Value()[1], translation.Value()[2]},
                 .rotation = {rotation.Value()[0], rotation.Value()[1], rotation.Value()[2], rotation.Value()[3]},
                 .scale = {scale.Value()[0], scale.Value()[1], scale.Value()[2]}});
        }

        /** @brief Parses an exact source revision without introducing a prefab-specific version counter. */
        [[nodiscard]] Result<PrefabSourceRevision> ParseRevision(const Json &value) {
            if (!HasAllowedFields(value, {"projectVersion", "contentDigest"}))
                return Failed<PrefabSourceRevision>(PrefabErrors::DocumentInvalid, "Prefab source revision fields are not canonical.");
            auto version = ParseProjectVersion(value.at("projectVersion"));
            if (version.HasError())
                return Result<PrefabSourceRevision>::Failure(version.ErrorValue());
            auto digestText = ReadString(value.at("contentDigest"));
            if (digestText.HasError())
                return Result<PrefabSourceRevision>::Failure(digestText.ErrorValue());
            auto digest = ParseSha256(digestText.Value());
            if (digest.HasError())
                return Failed<PrefabSourceRevision>(PrefabErrors::DocumentInvalid, "Prefab source revision digest is not canonical.");
            return Result<PrefabSourceRevision>::Success(
                {.projectVersion = std::move(version).Value(), .contentDigest = std::move(digest).Value()});
        }

        /** @brief Parses bounded opaque component bytes from the canonical byte-array envelope. */
        [[nodiscard]] Result<std::vector<std::byte>> ParseComponentBytes(const Json &value) {
            if (!HasAllowedFields(value, {"bytes"}) || !value.at("bytes").is_array() ||
                value.at("bytes").size() > Gameplay::MaximumSerializedComponentBytes)
                return Failed<std::vector<std::byte>>(PrefabErrors::PayloadTooLarge, "Prefab component payload exceeds its byte bound.");
            const Json &encoded = value.at("bytes");
            std::vector<std::byte> bytes;
            bytes.reserve(encoded.size());
            for (const Json &entry : encoded) {
                auto byte = ReadUnsigned<std::uint16_t>(entry);
                if (byte.HasError())
                    return Failed<std::vector<std::byte>>(PrefabErrors::DocumentInvalid,
                                                          "Prefab component payload contains a non-byte value.");
                if (byte.Value() > std::numeric_limits<std::uint8_t>::max())
                    return Failed<std::vector<std::byte>>(PrefabErrors::DocumentInvalid,
                                                          "Prefab component payload contains an oversized byte.");
                bytes.emplace_back(static_cast<std::byte>(byte.Value()));
            }
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }

        /** @brief Adds semantic payload bytes without allowing an intermediate parser candidate to exceed policy. */
        [[nodiscard]] bool AddPayloadBytes(std::size_t &total, const std::size_t bytes, const std::size_t maximum) noexcept {
            if (bytes > maximum - total)
                return false;
            total += bytes;
            return true;
        }

        /** @brief Parses one opaque component envelope and accounts for its bounded dynamic bytes. */
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

        /** @brief Parses one object while bounding child payload counts before vector allocation. */
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

        /** @brief Parses and accounts for the bounded Asset Registry dependency list. */
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

        /** @brief Parses and bounds the complete hierarchy array before semantic validation. */
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

        /** @brief Decodes a complete source DOM into one unpublished mutable candidate. */
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

        /** @brief Parses one bounded JSON DOM while retaining duplicate-key and depth diagnostics. */
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

        /** @brief Parses one complete source string and validates it before immutable publication. */
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
        return Detail::ParseDocumentImpl(source, limits, parseLimits);
    }

}  // namespace Horo::Prefab
