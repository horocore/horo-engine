#include "Horo/Scene/CookedSceneDefinition.h"

#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <functional>

namespace Horo::SceneCook {
    namespace SceneCookErrors {
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.scene.cook"},
                                          .code = ErrorCode{"scene.cook.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Cooked scene payload is malformed or has foreign identity.",
                                          .remediationHint = "Recook from the current validated project; do not activate this payload."};
        const ErrorCodeDescriptor Unsupported{.domain = ErrorDomainId{"horo.scene.cook"},
                                              .code = ErrorCode{"scene.cook.unsupported"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "A required component has no supported cooked scene representation.",
                                              .remediationHint =
                                                  "Supply the owning component's portable cook contract before releasing this scene."};
        const ErrorCodeDescriptor TooLarge{.domain = ErrorDomainId{"horo.scene.cook"},
                                           .code = ErrorCode{"scene.cook.too_large"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The complete scene exceeds its captured cook limits.",
                                           .remediationHint = "Reduce the scene or explicitly select a supported larger host policy."};
    }  // namespace SceneCookErrors

    namespace {
        using Json = nlohmann::json;
        using namespace SceneSource::Detail;

        /** @brief Copies the shared portable component fields without hidden defaults or disabled-component filtering. */
        template <typename Destination, typename Source> Destination CopyComponents(const Source &source) {
            return {.camera = source.camera,
                    .light = source.light,
                    .audioSource = source.audioSource,
                    .navigationSurface = source.navigationSurface,
                    .navigationRegion = source.navigationRegion,
                    .navigationModifier = source.navigationModifier,
                    .navigationLink = source.navigationLink,
                    .navigationAgent = source.navigationAgent,
                    .aiAgent = source.aiAgent,
                    .aiController = source.aiController,
                    .rigidBody = source.rigidBody,
                    .colliders = source.colliders,
                    .physicsConstraints = source.physicsConstraints,
                    .behaviors = source.behaviors,
                    .gameplayComponents = source.gameplayComponents};
        }

        /** @brief Bounds both encoding and decoding with exactly the same immutable policy. */
        bool WithinLimits(const std::size_t entities, const std::size_t dependencies, const CookedSceneLimits &limits) {
            return limits.maximumEntities > 0 && limits.maximumDependencies > 0 && limits.maximumBytes > 0 &&
                   entities <= limits.maximumEntities && dependencies <= limits.maximumDependencies;
        }

        /** @brief Encodes one supported source-free runtime entity using existing typed scalar/component codecs. */
        Result<Json> EncodeEntity(const Runtime::RuntimeEntityDefinition &entity) {
            if (entity.components.audioListener || entity.components.uiCanvas)
                return Result<Json>::Failure(MakeError(SceneCookErrors::Unsupported));
            return Result<Json>::Success(
                {{"id", entity.object.value},
                 {"parent", entity.parent ? Json(entity.parent->value) : Json(nullptr)},
                 {"transform", TransformJson(entity.localTransform).ToJson()},
                 {"primitiveMesh", entity.primitiveMesh ? PrimitiveJson(*entity.primitiveMesh).ToJson() : Json(nullptr)},
                 {"components", ComponentsJson(CopyComponents<SceneSource::SceneObjectComponentSet>(entity.components)).ToJson()}});
        }

        /** @brief Decodes an exact entity shape; authoring-only trigger and prefab data cannot enter runtime bytes. */
        Result<Runtime::RuntimeEntityDefinition> DecodeEntity(const Json &value) {
            if (!Foundation::HasAllowedFields(value, {"id", "parent", "transform", "primitiveMesh", "components"}) ||
                !value["id"].is_number_unsigned() || (!value["parent"].is_null() && !value["parent"].is_number_unsigned()))
                return Result<Runtime::RuntimeEntityDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            auto transform = ParseTransform(value["transform"]);
            auto components = ParseComponents(value["components"]);
            if (transform.HasError() || components.HasError())
                return Result<Runtime::RuntimeEntityDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            if (components.Value().triggerVolume)
                return Result<Runtime::RuntimeEntityDefinition>::Failure(MakeError(SceneCookErrors::Unsupported));
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            if (!value["primitiveMesh"].is_null()) {
                auto parsed = ParsePrimitive(value["primitiveMesh"]);
                if (parsed.HasError())
                    return Result<Runtime::RuntimeEntityDefinition>::Failure(parsed.ErrorValue());
                primitive = std::move(parsed).Value();
            }
            return Result<Runtime::RuntimeEntityDefinition>::Success(
                {.object = {value["id"].get<std::uint64_t>()},
                 .parent =
                     value["parent"].is_null() ? std::nullopt : std::optional{Runtime::SceneObjectId{value["parent"].get<std::uint64_t>()}},
                 .localTransform = transform.Value(),
                 .primitiveMesh = std::move(primitive),
                 .components = CopyComponents<Runtime::RuntimeComponentSet>(components.Value())});
        }

        /** @brief Validates and commits all entity and dependency values through the containing-scene owner. */
        Result<Runtime::RuntimeSceneDefinition> DecodeDocument(const Json &root, const Runtime::SceneDefinitionId scene,
                                                               const Runtime::SceneDefinitionRevision revision,
                                                               const CookedSceneLimits &limits) {
            if (!Foundation::HasAllowedFields(root, {"format", "scene", "revision", "entities", "dependencies"}) ||
                root["format"] != "horo.scene.runtime.v1" || !root["scene"].is_number_unsigned() || root["scene"] != scene.value ||
                !root["revision"].is_number_unsigned() || root["revision"] != revision.value || !root["entities"].is_array() ||
                !root["dependencies"].is_array())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            if (!WithinLimits(root["entities"].size(), root["dependencies"].size(), limits))
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::TooLarge));
            Runtime::SceneDefinitionBuilder builder{scene, revision};
            for (const auto &value : root["entities"]) {
                auto entity = DecodeEntity(value);
                if (entity.HasError())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(entity.ErrorValue());
                builder.Add(std::move(entity).Value());
            }
            for (const auto &value : root["dependencies"]) {
                if (!Foundation::HasAllowedFields(value, {"id", "type"}) || !value["id"].is_string() || !value["type"].is_string())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
                const auto id = Assets::AssetId::Parse(value["id"].get<std::string>());
                const auto type = Assets::AssetTypeId::Parse(value["type"].get<std::string>());
                if (id.HasError() || type.HasError() || type.Value().Value() == "core.prefab")
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
                if (auto required = builder.RequireAsset({id.Value(), type.Value()}); required.HasError())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(required.ErrorValue());
            }
            return std::move(builder).Build();
        }
    }  // namespace

    /** @copydoc EncodeCookedSceneDefinition */
    Result<std::vector<std::uint8_t>> EncodeCookedSceneDefinition(const Runtime::RuntimeSceneDefinition &definition,
                                                                  const CookedSceneLimits &limits) {
        if (!WithinLimits(definition.Entities().size(), definition.AssetDependencies().size(), limits))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(SceneCookErrors::TooLarge));
        Json entities = Json::array();
        for (const auto &entity : definition.Entities()) {
            auto encoded = EncodeEntity(entity);
            if (encoded.HasError())
                return Result<std::vector<std::uint8_t>>::Failure(encoded.ErrorValue());
            entities.push_back(std::move(encoded).Value());
        }
        Json dependencies = Json::array();
        for (const auto &dependency : definition.AssetDependencies()) {
            if (dependency.expectedType.Value() == "core.prefab")
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(SceneCookErrors::Invalid));
            dependencies.push_back({{"id", dependency.id.ToString()}, {"type", dependency.expectedType.Value()}});
        }
        const std::string encoded = Json{{"format", "horo.scene.runtime.v1"},
                                         {"scene", definition.Id().value},
                                         {"revision", definition.Revision().value},
                                         {"entities", std::move(entities)},
                                         {"dependencies", std::move(dependencies)}}
                                        .dump() +
                                    '\n';
        if (encoded.size() > limits.maximumBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(SceneCookErrors::TooLarge));
        return Result<std::vector<std::uint8_t>>::Success({encoded.begin(), encoded.end()});
    }

    /** @copydoc DecodeCookedSceneDefinition */
    Result<Runtime::RuntimeSceneDefinition> DecodeCookedSceneDefinition(const std::span<const std::uint8_t> bytes,
                                                                        const Runtime::SceneDefinitionId expectedScene,
                                                                        const Runtime::SceneDefinitionRevision expectedRevision,
                                                                        const CookedSceneLimits &limits) {
        if (bytes.empty() || bytes.size() > limits.maximumBytes)
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::TooLarge));
        try {
            Foundation::JsonParseGuard guard{32};
            const auto root = Json::parse(bytes.begin(), bytes.end(), std::ref(guard));
            if (guard.HasDuplicate() || guard.IsTooDeep())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            auto definition = DecodeDocument(root, expectedScene, expectedRevision, limits);
            if (definition.HasError())
                return definition;
            const auto canonical = EncodeCookedSceneDefinition(definition.Value(), limits);
            if (canonical.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(canonical.ErrorValue());
            if (!std::ranges::equal(canonical.Value(), bytes))
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            return definition;
        } catch (const Json::exception &) {
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
        }
    }
}  // namespace Horo::SceneCook
