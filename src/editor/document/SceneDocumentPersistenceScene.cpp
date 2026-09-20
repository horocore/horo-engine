#include "Horo/Foundation/Sha256.h"
#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <string>
#include <utility>

namespace Horo::Editor::ScenePersistenceDetail {
    [[nodiscard]] Json SceneJson(const SceneDocumentSnapshot &snapshot) {
        Json objects = Json::array();
        for (const SceneObjectSnapshot &object : snapshot.objects) {
            Json value{
                {"id", object.id.value},
                {"parent", object.parent.has_value() ? Json(object.parent->value) : Json(nullptr)},
                {"name", object.name},
                {"transform", TransformJson(object.localTransform)},
                {"components", ComponentsJson(object.components)},
                {"editor", {{"visible", object.editorState.visible}, {"locked", object.editorState.locked}}},
            };
            value["primitiveMesh"] = object.primitiveMesh.has_value() ? PrimitiveJson(*object.primitiveMesh) : Json(nullptr);
            value["meshAsset"] = object.meshAsset.has_value() ? Json(object.meshAsset->ToString()) : Json(nullptr);
            objects.push_back(std::move(value));
        }
        Json prefabInstances = Json::array();
        for (const ScenePrefabInstance &instance : snapshot.prefabInstances) {
            prefabInstances.push_back({
                {"instanceId", instance.instanceId.Value()},
                {"sourceAsset", instance.sourcePrefab.Asset().ToString()},
                {"parent", instance.parent.has_value() ? Json(instance.parent->value) : Json(nullptr)},
                {"rootTransform", TransformJson(instance.rootTransform)},
            });
        }
        return Json{{"schemaVersion", kSceneSchemaVersion},
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

    [[nodiscard]] Result<SceneObjectSnapshot> ParseSceneObjectSnapshot(const Json &value) {
        if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() || !value.contains("name") ||
            !value["name"].is_string() || !value.contains("parent") || !value.contains("transform") || !value.contains("primitiveMesh") ||
            !value.contains("components"))
            return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object schema is incomplete."));
        auto transform = ParseTransform(value["transform"]);
        auto components = ParseComponents(value["components"]);
        if (transform.HasError() || components.HasError())
            return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object values are invalid."));
        auto parent = ParseSceneObjectParent(value);
        auto primitive = ParseSceneObjectPrimitive(value);
        auto meshAsset = ParseSceneObjectMeshAsset(value);
        auto editorState = ParseSceneObjectEditorState(value);
        if (parent.HasError())
            return Result<SceneObjectSnapshot>::Failure(parent.ErrorValue());
        if (primitive.HasError())
            return Result<SceneObjectSnapshot>::Failure(primitive.ErrorValue());
        if (meshAsset.HasError())
            return Result<SceneObjectSnapshot>::Failure(meshAsset.ErrorValue());
        if (editorState.HasError())
            return Result<SceneObjectSnapshot>::Failure(editorState.ErrorValue());
        return Result<SceneObjectSnapshot>::Success(SceneObjectSnapshot{
            .id = SceneObjectId{value["id"].get<std::uint64_t>()},
            .parent = parent.Value(),
            .name = value["name"].get<std::string>(),
            .localTransform = transform.Value(),
            .primitiveMesh = primitive.Value(),
            .components = components.Value(),
            .meshAsset = meshAsset.Value(),
            .editorState = editorState.Value(),
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
        return Result<ScenePrefabInstance>::Success(
            ScenePrefabInstance{instanceId.Value(), sourcePrefab.Value(), parent, rootTransform.Value()});
    }

    [[nodiscard]] Result<ParsedScene> ParseScene(const std::string &contents) {
        try {
            const Json document = Json::parse(contents);
            if (!document.is_object() || !document.contains("schemaVersion") || document["schemaVersion"] != kSceneSchemaVersion ||
                !document.contains("objects") || !document["objects"].is_array() || document["objects"].size() > kMaximumSceneObjects ||
                (document.contains("prefabInstances") &&
                 (!document["prefabInstances"].is_array() || document["prefabInstances"].size() > kMaximumSceneObjects))) {
                return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Scene schema is unsupported or incomplete."));
            }

            ParsedScene scene;
            scene.objects.reserve(document["objects"].size());
            for (const Json &value : document["objects"]) {
                auto object = ParseSceneObjectSnapshot(value);
                if (object.HasError()) {
                    return Result<ParsedScene>::Failure(object.ErrorValue());
                }
                scene.objects.push_back(std::move(object).Value());
            }
            if (document.contains("prefabInstances")) {
                scene.prefabInstances.reserve(document["prefabInstances"].size());
                for (const Json &value : document["prefabInstances"]) {
                    auto instance = ParseScenePrefabInstance(value);
                    if (instance.HasError())
                        return Result<ParsedScene>::Failure(instance.ErrorValue());
                    scene.prefabInstances.push_back(std::move(instance).Value());
                }
            }
            return Result<ParsedScene>::Success(std::move(scene));
        } catch (const Json::exception &exception) {
            return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Invalid scene JSON: " + std::string{exception.what()}));
        }
    }

    [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
        const auto *begin = reinterpret_cast<const std::byte *>(value.data());
        return {begin, begin + value.size()};
    }

    [[nodiscard]] std::filesystem::path RecoveryPath(const std::filesystem::path &absoluteProjectRoot) {
        return absoluteProjectRoot / ".horo/local/recovery/default-scene.hororecovery";
    }

    [[nodiscard]] std::string SceneChecksum(const Json &scene) {
        const std::string canonical = scene.dump();
        return FormatSha256(
            ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(canonical.data()), canonical.size()}));
    }

    [[nodiscard]] SceneFileFingerprint Fingerprint(const std::string_view bytes) {
        return SceneFileFingerprint{
            .exists = true,
            .byteSize = bytes.size(),
            .checksum =
                FormatSha256(ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()})),
        };
    }
}  // namespace Horo::Editor::ScenePersistenceDetail
