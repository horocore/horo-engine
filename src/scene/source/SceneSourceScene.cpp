#include "Horo/Prefab/PrefabValueOverrides.h"
#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Horo::SceneSource::Detail {
    namespace {
        /** @brief Empties the depth-admitted DOM before flattening destruction; checked-iterator proxy allocations remain STL-specific. */
        void ClearSceneJson(Json &value) noexcept {
            if (value.is_structured())
                for (auto &child : value)
                    ClearSceneJson(child);
            value.clear();
        }

        /** @brief Owns only the completed, at-most-64-level parse tree; partial parser state remains upstream-owned. */
        struct BoundedSceneJson final {
            Json value;
            BoundedSceneJson() = default;
            BoundedSceneJson(const BoundedSceneJson &) = delete;
            BoundedSceneJson &operator=(const BoundedSceneJson &) = delete;
            BoundedSceneJson(BoundedSceneJson &&) = delete;
            BoundedSceneJson &operator=(BoundedSceneJson &&) = delete;

            ~BoundedSceneJson() {
                ClearSceneJson(value);
            }
        };
    }  // namespace

    [[nodiscard]] WireJson SceneJson(const SceneSourceView &snapshot) {
        WireJson objects = WireJson::array();
        for (const SceneObjectSnapshot &object : snapshot.objects) {
            WireJson value{
                {"id", object.id.value},
                {"parent", object.parent.has_value() ? WireJson(object.parent->value) : WireJson(nullptr)},
                {"name", object.name},
                {"transform", TransformJson(object.localTransform)},
                {"components", ComponentsJson(object.components)},
                {"editor", {{"visible", object.editorState.visible}, {"locked", object.editorState.locked}}},
            };
            value["primitiveMesh"] = object.primitiveMesh.has_value() ? PrimitiveJson(*object.primitiveMesh) : WireJson(nullptr);
            value["meshAsset"] = object.meshAsset.has_value() ? WireJson(object.meshAsset->ToString()) : WireJson(nullptr);
            if (object.generated)
                value["generated"] = SceneGeneratedOwnershipJson(*object.generated);
            objects.push_back(std::move(value));
        }
        WireJson prefabInstances = WireJson::array();
        for (const ScenePrefabInstance &instance : snapshot.prefabInstances) {
            WireJson value{
                {"instanceId", instance.instanceId.Value()},
                {"sourceAsset", instance.sourcePrefab.Asset().ToString()},
                {"parent", instance.parent.has_value() ? WireJson(instance.parent->value) : WireJson(nullptr)},
                {"rootTransform", TransformJson(instance.rootTransform)},
            };
            if (instance.valueOverrides) {
                const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
                auto encoded = Prefab::SerializePrefabValueOverrides(*instance.valueOverrides, limits);
                if (encoded.HasError())
                    throw std::invalid_argument("Scene contains invalid prefab value override storage.");
                value["valueOverrides"] = std::move(encoded).Value();
            }
            prefabInstances.push_back(std::move(value));
        }
        const bool character = std::ranges::any_of(snapshot.objects, [](const auto &object) {
            return object.components.characterController.has_value();
        });
        return WireJson{{"schemaVersion", character ? kCharacterSceneSchemaVersion : kSceneSchemaVersion},
                        {"objects", std::move(objects)},
                        {"prefabInstances", std::move(prefabInstances)}};
    }

    /** @brief Parses an optional authored parent identity. */
    [[nodiscard]] Result<std::optional<SceneObjectId>> ParseSceneObjectParent(const Json &value) {
        if (value["parent"].is_null())
            return Result<std::optional<SceneObjectId>>::Success(std::nullopt);
        if (!value["parent"].is_number_unsigned())
            return Result<std::optional<SceneObjectId>>::Failure(PersistenceError(SceneInvalid, "Scene object parent is invalid."));
        return Result<std::optional<SceneObjectId>>::Success(
            std::optional<SceneObjectId>{SceneObjectId{value["parent"].get<std::uint64_t>()}});
    }

    /** @brief Parses an optional primitive-mesh descriptor. */
    [[nodiscard]] Result<std::optional<PrimitiveMeshDescriptor>> ParseSceneObjectPrimitive(const Json &value) {
        if (value["primitiveMesh"].is_null())
            return Result<std::optional<PrimitiveMeshDescriptor>>::Success(std::nullopt);
        auto parsed = ParsePrimitive(value["primitiveMesh"]);
        if (parsed.HasError())
            return Result<std::optional<PrimitiveMeshDescriptor>>::Failure(parsed.ErrorValue());
        return Result<std::optional<PrimitiveMeshDescriptor>>::Success(std::optional<PrimitiveMeshDescriptor>{std::move(parsed).Value()});
    }

    /** @brief Parses an optional external mesh asset identity. */
    [[nodiscard]] Result<std::optional<Assets::AssetId>> ParseSceneObjectMeshAsset(const Json &value) {
        if (!value.contains("meshAsset") || value["meshAsset"].is_null())
            return Result<std::optional<Assets::AssetId>>::Success(std::nullopt);
        if (!value["meshAsset"].is_string())
            return Result<std::optional<Assets::AssetId>>::Failure(
                PersistenceError(SceneInvalid, "Scene object mesh asset identity is invalid."));
        auto parsed = Assets::AssetId::Parse(value["meshAsset"].get<std::string>());
        if (parsed.HasError())
            return Result<std::optional<Assets::AssetId>>::Failure(
                PersistenceError(SceneInvalid, "Scene object mesh asset identity is invalid."));
        return Result<std::optional<Assets::AssetId>>::Success(std::optional<Assets::AssetId>{std::move(parsed).Value()});
    }

    /** @brief Parses the optional editor visibility and lock state. */
    [[nodiscard]] Result<SceneObjectEditorState> ParseSceneObjectEditorState(const Json &value) {
        if (!value.contains("editor"))
            return Result<SceneObjectEditorState>::Success(SceneObjectEditorState{});
        const Json &editor = value["editor"];
        if (!editor.is_object() || !editor.contains("visible") || !editor["visible"].is_boolean() || !editor.contains("locked") ||
            !editor["locked"].is_boolean())
            return Result<SceneObjectEditorState>::Failure(PersistenceError(SceneInvalid, "Scene object editor state is invalid."));
        return Result<SceneObjectEditorState>::Success(
            SceneObjectEditorState{.visible = editor["visible"].get<bool>(), .locked = editor["locked"].get<bool>()});
    }

    [[nodiscard]] Result<SceneObjectSnapshot> ParseSceneObjectSnapshot(const Json &value, bool &unsupportedComponents) {
        unsupportedComponents = false;
        if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() || !value.contains("name") ||
            !value["name"].is_string() || !value.contains("parent") || !value.contains("transform") || !value.contains("primitiveMesh") ||
            !value.contains("components"))
            return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object schema is incomplete."));
        auto transform = ParseTransform(value["transform"]);
        std::size_t recognized{};
        auto components = ParseComponents(value["components"], recognized);
        if (transform.HasError() || components.HasError())
            return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object values are invalid."));
        auto parent = ParseSceneObjectParent(value);
        auto primitive = ParseSceneObjectPrimitive(value);
        auto meshAsset = ParseSceneObjectMeshAsset(value);
        auto editorState = ParseSceneObjectEditorState(value);
        auto generated = ParseSceneGeneratedOwnership(value);
        if (parent.HasError())
            return Result<SceneObjectSnapshot>::Failure(parent.ErrorValue());
        if (primitive.HasError())
            return Result<SceneObjectSnapshot>::Failure(primitive.ErrorValue());
        if (meshAsset.HasError())
            return Result<SceneObjectSnapshot>::Failure(meshAsset.ErrorValue());
        if (editorState.HasError())
            return Result<SceneObjectSnapshot>::Failure(editorState.ErrorValue());
        if (generated.HasError())
            return Result<SceneObjectSnapshot>::Failure(generated.ErrorValue());
        unsupportedComponents = recognized != value["components"].size();
        return Result<SceneObjectSnapshot>::Success(SceneObjectSnapshot{
            .id = SceneObjectId{value["id"].get<std::uint64_t>()},
            .parent = parent.Value(),
            .name = value["name"].get<std::string>(),
            .localTransform = transform.Value(),
            .primitiveMesh = primitive.Value(),
            .components = components.Value(),
            .meshAsset = meshAsset.Value(),
            .editorState = editorState.Value(),
            .generated = std::move(generated).Value(),
        });
    }

    [[nodiscard]] Result<ScenePrefabInstance> ParseScenePrefabInstance(const Json &value) {
        if (!value.is_object() || !value.contains("instanceId") || !value["instanceId"].is_number_unsigned() ||
            !value.contains("sourceAsset") || !value["sourceAsset"].is_string() || !value.contains("parent") ||
            !value.contains("rootTransform")) {
            return Result<ScenePrefabInstance>::Failure(PersistenceError(SceneInvalid, "Scene prefab instance schema is incomplete."));
        }
        auto instanceId = Prefab::PrefabInstanceId::Create(value["instanceId"].get<std::uint64_t>());
        auto assetId = Assets::AssetId::Parse(value["sourceAsset"].get<std::string>());
        auto rootTransform = ParseTransform(value["rootTransform"]);
        if (instanceId.HasError() || assetId.HasError() || rootTransform.HasError()) {
            return Result<ScenePrefabInstance>::Failure(
                PersistenceError(SceneInvalid, "Scene prefab instance identity, source, or root transform is invalid."));
        }
        auto sourcePrefab = Prefab::PrefabAssetReference::Create(assetId.Value());
        if (sourcePrefab.HasError()) {
            return Result<ScenePrefabInstance>::Failure(PersistenceError(SceneInvalid, "Scene prefab source asset identity is invalid."));
        }
        std::optional<SceneObjectId> parent;
        if (!value["parent"].is_null()) {
            if (!value["parent"].is_number_unsigned()) {
                return Result<ScenePrefabInstance>::Failure(PersistenceError(SceneInvalid, "Scene prefab instance parent is invalid."));
            }
            parent = SceneObjectId{value["parent"].get<std::uint64_t>()};
        }
        std::optional<Prefab::PrefabValueOverrideSet> overrides;
        if (value.contains("valueOverrides")) {
            if (!value["valueOverrides"].is_string())
                return Result<ScenePrefabInstance>::Failure(
                    PersistenceError(SceneInvalid, "Prefab override envelope must contain canonical bytes."));
            auto parsed = Prefab::ParsePrefabValueOverrides(value["valueOverrides"].get_ref<const std::string &>(),
                                                            Prefab::PrefabLimitProfile::Create({}).Value());
            if (parsed.HasError())
                return Result<ScenePrefabInstance>::Failure(parsed.ErrorValue());
            if (parsed.Value().sourcePrefab != assetId.Value())
                return Result<ScenePrefabInstance>::Failure(
                    PersistenceError(SceneInvalid, "Prefab override source identity disagrees with placement."));
            overrides = std::move(parsed).Value();
        }
        return Result<ScenePrefabInstance>::Success(
            ScenePrefabInstance{instanceId.Value(), sourcePrefab.Value(), parent, rootTransform.Value(), std::move(overrides)});
    }

    namespace {
        /** @brief Admits only complete bounded legacy or Character-capable document roots. */
        bool InvalidSceneRoot(const Json &document) {
            return !document.is_object() || !document.contains("schemaVersion") ||
                   (document["schemaVersion"] != kSceneSchemaVersion && document["schemaVersion"] != kCharacterSceneSchemaVersion) ||
                   !document.contains("objects") || !document["objects"].is_array() || document["objects"].size() > kMaximumSceneObjects ||
                   (document.contains("prefabInstances") &&
                    (!document["prefabInstances"].is_array() || document["prefabInstances"].size() > kMaximumSceneObjects));
        }
    }  // namespace

    [[nodiscard]] Result<ParsedScene> ParseScene(const std::string_view contents) {
        try {
            Foundation::JsonParseGuard guard{64};
            BoundedSceneJson parsed;
            parsed.value = guard.Parse(contents.begin(), contents.end());
            if (guard.IsTooDeep() || guard.HasDuplicate())
                return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Scene JSON nesting or duplicate keys are invalid."));
            const Json &document = parsed.value;
            if (InvalidSceneRoot(document))
                return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Scene schema is unsupported or incomplete."));

            ParsedScene scene;
            scene.objects.reserve(document["objects"].size());
            for (const Json &value : document["objects"]) {
                bool unsupported{};
                auto object = ParseSceneObjectSnapshot(value, unsupported);
                if (object.HasError()) {
                    return Result<ParsedScene>::Failure(object.ErrorValue());
                }
                if (unsupported)
                    scene.unsupportedComponentOwners.push_back(object.Value().id);
                scene.objects.push_back(std::move(object).Value());
            }
            const bool character = std::ranges::any_of(scene.objects, [](const auto &object) {
                return object.components.characterController.has_value();
            });
            if (character && document["schemaVersion"] != kCharacterSceneSchemaVersion)
                return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Character components require Scene source schema2."));
            if (document.contains("prefabInstances")) {
                scene.prefabInstances.reserve(document["prefabInstances"].size());
                for (const Json &value : document["prefabInstances"]) {
                    auto instance = ParseScenePrefabInstance(value);
                    if (instance.HasError())
                        return Result<ParsedScene>::Failure(instance.ErrorValue());
                    scene.prefabInstances.push_back(std::move(instance).Value());
                }
            }
            std::ranges::sort(scene.unsupportedComponentOwners, {}, &SceneObjectId::value);
            const auto duplicates = std::ranges::unique(scene.unsupportedComponentOwners);
            scene.unsupportedComponentOwners.erase(duplicates.begin(), duplicates.end());
            return Result<ParsedScene>::Success(std::move(scene));
        } catch (const Json::exception &exception) {
            return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Invalid scene JSON: " + std::string{exception.what()}));
        }
    }

}  // namespace Horo::SceneSource::Detail
