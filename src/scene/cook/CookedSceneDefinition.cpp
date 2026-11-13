#include "Horo/Scene/CookedSceneDefinition.h"

#include "Horo/Foundation/Sha256.h"
#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <functional>
#include <optional>

namespace Horo::SceneCook {
    /** @copydoc SceneDefinitionIdForAsset */
    Runtime::SceneDefinitionId SceneDefinitionIdForAsset(const Assets::AssetId asset) {
        const auto digest = ComputeSha256(std::as_bytes(std::span{asset.Bytes()}));
        std::uint64_t value{};
        for (std::size_t index = 0; index < 8; ++index)
            value |= static_cast<std::uint64_t>(digest.bytes[index]) << (8U * index);
        return Runtime::SceneDefinitionId{value == 0 ? 1 : value};
    }

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
                    .animation = source.animation,
                    .light = source.light,
                    .audioSource = source.audioSource,
                    .audioListener = source.audioListener,
                    .navigationSurface = source.navigationSurface,
                    .navigationRegion = source.navigationRegion,
                    .navigationModifier = source.navigationModifier,
                    .navigationLink = source.navigationLink,
                    .navigationAgent = source.navigationAgent,
                    .aiAgent = source.aiAgent,
                    .aiController = source.aiController,
                    .characterController = source.characterController,
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
            if (entity.components.uiCanvas)
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
            std::size_t recognizedComponents{};
            auto components = ParseComponents(value["components"], recognizedComponents);
            if (transform.HasError() || components.HasError())
                return Result<Runtime::RuntimeEntityDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
            if (recognizedComponents != value["components"].size() || components.Value().triggerVolume)
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

        /** @brief Decodes one exact portable proof value; the Scene builder owns canonical correspondence checks. */
        Result<Runtime::TriggerVolumeMigrationEvidence> DecodeMigration(const Json &value) {
            if (const bool identityFields = value.is_object() && value.contains("object") && value.contains("body") &&
                                            value.contains("collider") && value["object"].is_number_unsigned() &&
                                            value["body"].is_number_unsigned() && value["collider"].is_number_unsigned();
                !identityFields || !Foundation::HasAllowedFields(value, {"object", "body", "collider", "schema", "profile", "material",
                                                                         "type", "revision", "payload"}))
                return Result<Runtime::TriggerVolumeMigrationEvidence>::Failure(MakeError(SceneCookErrors::Invalid));
            if (!value["schema"].is_number_unsigned() || !value["revision"].is_number_unsigned() || !value["profile"].is_string() ||
                !value["material"].is_string() || !value["type"].is_string() || !value["payload"].is_string())
                return Result<Runtime::TriggerVolumeMigrationEvidence>::Failure(MakeError(SceneCookErrors::Invalid));
            const auto profile = Physics::CollisionProfileId::Parse(value["profile"].get<std::string>());
            const auto material = Assets::AssetId::Parse(value["material"].get<std::string>());
            const auto type = Assets::AssetTypeId::Parse(value["type"].get<std::string>());
            const auto digest = ParseSha256(value["payload"].get<std::string>());
            if (profile.HasError() || material.HasError() || type.HasError() || digest.HasError())
                return Result<Runtime::TriggerVolumeMigrationEvidence>::Failure(MakeError(SceneCookErrors::Invalid));
            return Result<Runtime::TriggerVolumeMigrationEvidence>::Success({{value["object"].get<std::uint64_t>()},
                                                                             {value["body"].get<std::uint64_t>()},
                                                                             {value["collider"].get<std::uint64_t>()},
                                                                             {value["schema"].get<std::uint64_t>(),
                                                                              profile.Value(),
                                                                              {material.Value(), type.Value()},
                                                                              value["revision"].get<std::uint64_t>(),
                                                                              digest.Value()}});
        }

        /** @brief Selects only the schema required by the complete typed component inventory. */
        std::string_view FormatFor(const Runtime::RuntimeSceneDefinition &definition) {
            const bool animation = std::ranges::any_of(definition.Entities(), [](const auto &entity) {
                return entity.components.animation.has_value();
            });
            return animation ? AnimationCookedSceneFormat : CookedSceneFormat;
        }

        /** @brief Checks exact top-level members and closed format spelling before indexed payload reads. */
        bool ValidEnvelopeShape(const Json &root) {
            if (!Foundation::HasAllowedFields(root, {"format", "scene", "revision", "entities", "dependencies", "triggerMigration"}) ||
                !root["format"].is_string())
                return false;
            const auto &format = root["format"].get_ref<const Json::string_t &>();
            return format == CookedSceneFormat || format == AnimationCookedSceneFormat;
        }

        /** @brief Preserves exact selected Scene/revision identity and typed collection admission. */
        bool ValidEnvelopeIdentity(const Json &root, Runtime::SceneDefinitionId scene, Runtime::SceneDefinitionRevision revision) {
            return root["scene"].is_number_unsigned() && root["scene"] == scene.value && root["revision"].is_number_unsigned() &&
                   root["revision"] == revision.value && root["entities"].is_array() && root["dependencies"].is_array() &&
                   root["triggerMigration"].is_array();
        }

        /** @brief Decodes every entity through the single canonical component projection. */
        Result<void> ReadEntities(Runtime::SceneDefinitionBuilder &builder, const Json &values) {
            for (const auto &value : values) {
                auto entity = DecodeEntity(value);
                if (entity.HasError())
                    return Result<void>::Failure(entity.ErrorValue());
                builder.Add(std::move(entity).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Admits exact declared asset identities; no unexpanded prefab may enter a source-free Scene. */
        Result<void> ReadDependencies(Runtime::SceneDefinitionBuilder &builder, const Json &values) {
            for (const auto &value : values) {
                if (!Foundation::HasAllowedFields(value, {"id", "type"}) || !value["id"].is_string() || !value["type"].is_string())
                    return Result<void>::Failure(MakeError(SceneCookErrors::Invalid));
                const auto id = Assets::AssetId::Parse(value["id"].get<std::string>());
                const auto type = Assets::AssetTypeId::Parse(value["type"].get<std::string>());
                if (id.HasError() || type.HasError() || type.Value().Value() == "core.prefab")
                    return Result<void>::Failure(MakeError(SceneCookErrors::Invalid));
                if (auto required = builder.RequireAsset({id.Value(), type.Value()}); required.HasError())
                    return required;
            }
            return Result<void>::Success();
        }

        /** @brief Retains complete migration evidence before final canonical builder validation. */
        Result<void> ReadMigration(Runtime::SceneDefinitionBuilder &builder, const Json &values) {
            for (const auto &value : values) {
                auto evidence = DecodeMigration(value);
                if (evidence.HasError())
                    return Result<void>::Failure(evidence.ErrorValue());
                if (auto admitted = builder.RequireTriggerVolume(std::move(evidence).Value()); admitted.HasError())
                    return admitted;
            }
            return Result<void>::Success();
        }

        /** @brief Validates and commits all entity and dependency values through the containing-scene owner. */
        Result<Runtime::RuntimeSceneDefinition> DecodeDocument(const Json &root, const Runtime::SceneDefinitionId scene,
                                                               const Runtime::SceneDefinitionRevision revision,
                                                               const CookedSceneLimits &limits) {
            using Return = Result<Runtime::RuntimeSceneDefinition>;
            if (!ValidEnvelopeShape(root) || !ValidEnvelopeIdentity(root, scene, revision))
                return Return::Failure(MakeError(SceneCookErrors::Invalid));
            if (!WithinLimits(root["entities"].size(), root["dependencies"].size(), limits) ||
                root["triggerMigration"].size() > root["entities"].size())
                return Return::Failure(MakeError(SceneCookErrors::TooLarge));
            Runtime::SceneDefinitionBuilder builder{scene, revision};
            if (auto entities = ReadEntities(builder, root["entities"]); entities.HasError())
                return Return::Failure(entities.ErrorValue());
            if (auto dependencies = ReadDependencies(builder, root["dependencies"]); dependencies.HasError())
                return Return::Failure(dependencies.ErrorValue());
            if (auto migration = ReadMigration(builder, root["triggerMigration"]); migration.HasError())
                return Return::Failure(migration.ErrorValue());
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
        Json migration = Json::array();
        for (const auto &evidence : definition.TriggerVolumeEvidence()) {
            const auto &binding = evidence.binding;
            migration.push_back({{"object", evidence.object.value},
                                 {"body", evidence.body.value},
                                 {"collider", evidence.collider.value},
                                 {"schema", binding.collisionSchemaFingerprint},
                                 {"profile", binding.profile.ToString()},
                                 {"material", binding.material.id.ToString()},
                                 {"type", binding.material.expectedType.Value()},
                                 {"revision", binding.materialSourceRevision},
                                 {"payload", FormatSha256(binding.materialPayloadDigest)}});
        }
        const std::string encoded =
            Json{{"format", FormatFor(definition)}, {"scene", definition.Id().value},          {"revision", definition.Revision().value},
                 {"entities", std::move(entities)}, {"dependencies", std::move(dependencies)}, {"triggerMigration", std::move(migration)}}
                .dump() +
            '\n';
        if (encoded.size() > limits.maximumBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(SceneCookErrors::TooLarge));
        return Result<std::vector<std::uint8_t>>::Success({encoded.begin(), encoded.end()});
    }

    namespace {
        /** @brief Shares canonical parsing and complete typed admission for exact-revision and verified-asset hosts. */
        Result<Runtime::RuntimeSceneDefinition> DecodePayload(const std::span<const std::uint8_t> bytes,
                                                              const Runtime::SceneDefinitionId expectedScene,
                                                              const std::optional<Runtime::SceneDefinitionRevision> expectedRevision,
                                                              const CookedSceneLimits &limits) {
            if (bytes.empty() || bytes.size() > limits.maximumBytes)
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::TooLarge));
            try {
                Foundation::JsonParseGuard guard{32};
                const auto root = guard.Parse(bytes.begin(), bytes.end());
                if (guard.HasDuplicate() || guard.IsTooDeep() || !root.is_object() || !root.contains("revision") ||
                    !root["revision"].is_number_unsigned() || root["revision"].get<std::uint64_t>() == 0)
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(SceneCookErrors::Invalid));
                const auto revision = expectedRevision.value_or(Runtime::SceneDefinitionRevision{root["revision"].get<std::uint64_t>()});
                auto definition = DecodeDocument(root, expectedScene, revision, limits);
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
    }  // namespace

    /** @copydoc DecodeCookedSceneDefinition */
    Result<Runtime::RuntimeSceneDefinition> DecodeCookedSceneDefinition(const std::span<const std::uint8_t> bytes,
                                                                        const Runtime::SceneDefinitionId expectedScene,
                                                                        const Runtime::SceneDefinitionRevision expectedRevision,
                                                                        const CookedSceneLimits &limits) {
        return DecodePayload(bytes, expectedScene, expectedRevision, limits);
    }

    /** @copydoc DecodeCookedSceneWithEmbeddedRevision */
    Result<Runtime::RuntimeSceneDefinition> DecodeCookedSceneWithEmbeddedRevision(const std::span<const std::uint8_t> bytes,
                                                                                  const Runtime::SceneDefinitionId expectedScene,
                                                                                  const CookedSceneLimits &limits) {
        return DecodePayload(bytes, expectedScene, std::nullopt, limits);
    }
}  // namespace Horo::SceneCook
