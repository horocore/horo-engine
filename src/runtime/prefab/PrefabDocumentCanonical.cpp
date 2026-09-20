#include "Horo/Prefab/PrefabDocument.h"
#include "PrefabDocumentSerializationInternal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Prefab {
    namespace {
        using OrderedJson = nlohmann::ordered_json;
        using Detail::Failed;
        using Detail::IsCanonicalProjectVersion;

        /** @brief Normalizes negative zero so equivalent finite floating values have one JSON spelling. */
        [[nodiscard]] float CanonicalFloat(const float value) noexcept {
            return value == 0.0F ? 0.0F : value;
        }

        /** @brief Encodes one transform with fixed field and component ordering. */
        [[nodiscard]] OrderedJson EncodeTransform(const Math::Transform &transform) {
            return OrderedJson{{"translation",
                                OrderedJson::array({CanonicalFloat(transform.translation.x), CanonicalFloat(transform.translation.y),
                                                    CanonicalFloat(transform.translation.z)})},
                               {"rotation",
                                OrderedJson::array({CanonicalFloat(transform.rotation.x), CanonicalFloat(transform.rotation.y),
                                                    CanonicalFloat(transform.rotation.z), CanonicalFloat(transform.rotation.w)})},
                               {"scale", OrderedJson::array({CanonicalFloat(transform.scale.x), CanonicalFloat(transform.scale.y),
                                                             CanonicalFloat(transform.scale.z)})}};
        }

        /** @brief Encodes one exact source revision. */
        [[nodiscard]] OrderedJson EncodeRevision(const PrefabSourceRevision &revision) {
            return OrderedJson{{"projectVersion", Application::FormatHoroVersion(revision.projectVersion)},
                               {"contentDigest", FormatSha256(revision.contentDigest)}};
        }

        /** @brief Encodes one opaque payload byte-for-byte as unsigned JSON byte values. */
        [[nodiscard]] OrderedJson EncodeComponentBytes(const std::vector<std::byte> &bytes) {
            OrderedJson encoded = OrderedJson::array();
            for (const std::byte byte : bytes)
                encoded.push_back(std::to_integer<unsigned int>(byte));
            return OrderedJson{{"bytes", std::move(encoded)}};
        }

        /** @brief Encodes one component envelope with the only currently admitted portable encoding. */
        [[nodiscard]] Result<OrderedJson> EncodeComponent(const RawComponentPayload &component) {
            if (component.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson)
                return Failed<OrderedJson>(PrefabErrors::UnsupportedPrefabSchema, "Prefab component encoding is unsupported.");
            return Result<OrderedJson>::Success(OrderedJson{{"instanceId", component.instance.Value()},
                                                            {"typeId", component.component.typeId.Value()},
                                                            {"schemaVersion", component.component.schemaVersion},
                                                            {"encoding", "canonicalJson"},
                                                            {"payload", EncodeComponentBytes(component.component.payload)}});
        }

        /** @brief Encodes one typed behavior value with an explicit stable kind tag. */
        template <typename Value> [[nodiscard]] OrderedJson EncodeTypedBehaviorValue(const Value &typed) {
            if constexpr (std::is_same_v<Value, std::monostate>)
                return OrderedJson{{"type", "null"}, {"value", nullptr}};
            if constexpr (std::is_same_v<Value, bool>)
                return OrderedJson{{"type", "bool"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, std::int64_t>)
                return OrderedJson{{"type", "int"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, double>)
                return OrderedJson{{"type", "number"}, {"value", typed == 0.0 ? 0.0 : typed}};
            if constexpr (std::is_same_v<Value, std::string>)
                return OrderedJson{{"type", "string"}, {"value", typed}};
            if constexpr (std::is_same_v<Value, Math::Vec2>)
                return OrderedJson{{"type", "vec2"}, {"value", OrderedJson::array({CanonicalFloat(typed.x), CanonicalFloat(typed.y)})}};
            if constexpr (std::is_same_v<Value, Math::Vec3>)
                return OrderedJson{{"type", "vec3"},
                                   {"value",
                                    OrderedJson::array({CanonicalFloat(typed.x), CanonicalFloat(typed.y), CanonicalFloat(typed.z)})}};
            if constexpr (std::is_same_v<Value, Math::Quaternion>)
                return OrderedJson{{"type", "quaternion"},
                                   {"value", OrderedJson::array({CanonicalFloat(typed.x), CanonicalFloat(typed.y), CanonicalFloat(typed.z),
                                                                 CanonicalFloat(typed.w)})}};
            return OrderedJson{};
        }

        /** @brief Dispatches one variant-held behavior value to its stable kind encoder. */
        [[nodiscard]] OrderedJson EncodeBehaviorValue(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([]<typename Value>(const Value &typed) {
                return EncodeTypedBehaviorValue(typed);
            }, value);
        }

        /** @brief Encodes one behavior and sorts fields by stable field name. */
        [[nodiscard]] OrderedJson EncodeBehavior(const Gameplay::BehaviorComponent &behavior) {
            std::vector<const Gameplay::BehaviorField *> fields;
            fields.reserve(behavior.fields.size());
            for (const Gameplay::BehaviorField &field : behavior.fields)
                fields.push_back(&field);
            std::ranges::sort(fields, [](const auto *lhs, const auto *rhs) {
                return lhs->name < rhs->name;
            });
            OrderedJson encodedFields = OrderedJson::array();
            for (const Gameplay::BehaviorField *field : fields)
                encodedFields.push_back(OrderedJson{{"name", field->name}, {"value", EncodeBehaviorValue(field->value)}});
            return OrderedJson{{"instanceId", behavior.instanceId.value},
                               {"typeId", behavior.typeId.Value()},
                               {"schemaVersion", behavior.schemaVersion},
                               {"enabled", behavior.enabled},
                               {"fields", std::move(encodedFields)}};
        }

        /** @brief Encodes one hierarchy object with stable payload ordering. */
        [[nodiscard]] Result<OrderedJson> EncodeObject(const PrefabObjectNode &object) {
            std::vector<const RawComponentPayload *> components;
            components.reserve(object.components.size());
            for (const RawComponentPayload &component : object.components)
                components.push_back(&component);
            std::ranges::sort(components, [](const auto *lhs, const auto *rhs) {
                return lhs->instance.Value() < rhs->instance.Value();
            });
            OrderedJson encodedComponents = OrderedJson::array();
            for (const RawComponentPayload *component : components) {
                auto encoded = EncodeComponent(*component);
                if (encoded.HasError())
                    return Result<OrderedJson>::Failure(encoded.ErrorValue());
                encodedComponents.push_back(std::move(encoded).Value());
            }

            std::vector<const Gameplay::BehaviorComponent *> behaviors;
            behaviors.reserve(object.behaviors.size());
            for (const Gameplay::BehaviorComponent &behavior : object.behaviors)
                behaviors.push_back(&behavior);
            std::ranges::sort(behaviors, [](const auto *lhs, const auto *rhs) {
                return lhs->instanceId.value < rhs->instanceId.value;
            });
            OrderedJson encodedBehaviors = OrderedJson::array();
            for (const Gameplay::BehaviorComponent *behavior : behaviors)
                encodedBehaviors.push_back(EncodeBehavior(*behavior));

            return Result<OrderedJson>::Success(
                OrderedJson{{"localId", object.localId.value},
                            {"parentLocalId", object.parentLocalId ? OrderedJson(object.parentLocalId->value) : OrderedJson(nullptr)},
                            {"name", object.name},
                            {"localTransform", EncodeTransform(object.localTransform)},
                            {"components", std::move(encodedComponents)},
                            {"behaviors", std::move(encodedBehaviors)}});
        }

        /** @brief Produces a deterministic parent-before-child order independent of sibling source order. */
        [[nodiscard]] std::vector<const PrefabObjectNode *> CanonicalObjectOrder(const std::vector<PrefabObjectNode> &objects) {
            std::vector<const PrefabObjectNode *> ordered;
            ordered.reserve(objects.size());
            std::unordered_set<std::uint32_t> emitted;
            emitted.reserve(objects.size());
            while (ordered.size() < objects.size()) {
                const PrefabObjectNode *next = nullptr;
                for (const PrefabObjectNode &object : objects) {
                    if (emitted.contains(object.localId.value) || (object.parentLocalId && !emitted.contains(object.parentLocalId->value)))
                        continue;
                    if (next == nullptr || object.localId.value < next->localId.value)
                        next = &object;
                }
                if (next == nullptr)
                    return {};
                emitted.emplace(next->localId.value);
                ordered.push_back(next);
            }
            return ordered;
        }

        /** @brief Encodes one nested placement in persisted identity order. */
        [[nodiscard]] OrderedJson EncodePlacement(const NestedPrefabPlacement &placement) {
            const bool mountsAtRoot = !placement.parentLocalId || placement.parentLocalId->IsRoot();
            return OrderedJson{{"placementLocalId", placement.placementLocalId.value},
                               {"parentLocalId", mountsAtRoot ? OrderedJson(nullptr) : OrderedJson(placement.parentLocalId->value)},
                               {"sourceAsset", placement.sourcePrefab.Asset().ToString()},
                               {"authoredAgainst", EncodeRevision(placement.authoredAgainst)},
                               {"localRootTransform", EncodeTransform(placement.localRootTransform)}};
        }

        /** @brief Encodes optional composition without allowing record order to select precedence. */
        [[nodiscard]] Result<OrderedJson> EncodeComposition(const PrefabComposition &composition) {
            if (composition.variantParent) {
                if (!composition.variantAuthoredAgainst)
                    return Failed<OrderedJson>(PrefabErrors::CompositionInvalid, "Prefab variant revision is missing.");
                return Result<OrderedJson>::Success(
                    OrderedJson{{"variantParent", composition.variantParent->Asset().ToString()},
                                {"variantAuthoredAgainst", EncodeRevision(*composition.variantAuthoredAgainst)}});
            }
            if (composition.variantAuthoredAgainst || composition.nestedPlacements.empty())
                return Failed<OrderedJson>(PrefabErrors::CompositionInvalid, "Prefab composition is not canonical.");
            std::vector<const NestedPrefabPlacement *> placements;
            placements.reserve(composition.nestedPlacements.size());
            for (const NestedPrefabPlacement &placement : composition.nestedPlacements)
                placements.push_back(&placement);
            std::ranges::sort(placements, [](const auto *lhs, const auto *rhs) {
                return lhs->placementLocalId.value < rhs->placementLocalId.value;
            });
            OrderedJson encodedPlacements = OrderedJson::array();
            for (const NestedPrefabPlacement *placement : placements)
                encodedPlacements.push_back(EncodePlacement(*placement));
            return Result<OrderedJson>::Success(OrderedJson{{"nestedPlacements", std::move(encodedPlacements)}});
        }

        /** @brief Encodes a validated immutable document into its one canonical JSON byte sequence. */
        [[nodiscard]] Result<std::string> EncodeDocument(const PrefabDocument &document) {
            const PrefabDocumentData &data = document.Data();
            if (!data.assetId.IsValid() || !IsCanonicalProjectVersion(data.projectVersion))
                return Failed<std::string>(PrefabErrors::DocumentInvalid, "Prefab document metadata is not canonical.");
            OrderedJson encodedObjects = OrderedJson::array();
            const auto objects = CanonicalObjectOrder(data.objects);
            if (objects.size() != data.objects.size())
                return Failed<std::string>(PrefabErrors::HierarchyInvalid, "Prefab hierarchy cannot be canonically ordered.");
            for (const PrefabObjectNode *object : objects) {
                auto encoded = EncodeObject(*object);
                if (encoded.HasError())
                    return Result<std::string>::Failure(encoded.ErrorValue());
                encodedObjects.push_back(std::move(encoded).Value());
            }

            std::vector<Assets::AssetId> references = data.referencedAssets;
            std::ranges::sort(references);
            OrderedJson encodedReferences = OrderedJson::array();
            for (const Assets::AssetId &reference : references)
                encodedReferences.push_back(reference.ToString());

            OrderedJson root{{"projectVersion", Application::FormatHoroVersion(data.projectVersion)},
                             {"assetId", data.assetId.ToString()},
                             {"objects", std::move(encodedObjects)}};
            if (data.composition) {
                auto composition = EncodeComposition(*data.composition);
                if (composition.HasError())
                    return Result<std::string>::Failure(composition.ErrorValue());
                root["composition"] = std::move(composition).Value();
            }
            root["referencedAssets"] = std::move(encodedReferences);
            return Result<std::string>::Success(root.dump(2) + "\n");
        }
    }  // namespace

    /** @copydoc PrefabDocument::SerializeCanonical */
    Result<std::string> PrefabDocument::SerializeCanonical() const {
        return EncodeDocument(*this);
    }

}  // namespace Horo::Prefab
