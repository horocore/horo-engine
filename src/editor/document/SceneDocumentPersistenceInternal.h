#pragma once

/**
 * @file SceneDocumentPersistenceInternal.h
 * @brief Private shared contracts for scene persistence codecs and I/O.
 */

#include "editor/document/SceneDocumentPersistence.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor::ScenePersistenceDetail {
    using Json = nlohmann::json;

    inline constexpr std::uint32_t kSceneSchemaVersion = 1;
    inline constexpr std::uintmax_t kMaximumProjectMetadataBytes = 64U * 1024U;
    inline constexpr std::uintmax_t kMaximumSceneBytes = 16U * 1024U * 1024U;
    inline constexpr std::uintmax_t kMaximumRecoveryBytes = 20U * 1024U * 1024U;
    inline constexpr std::size_t kMaximumSceneObjects = 100'000;

    extern const ErrorCodeDescriptor ScenePathInvalid;
    extern const ErrorCodeDescriptor SceneReadFailed;
    extern const ErrorCodeDescriptor SceneInvalid;

    /** @brief Creates a persistence-domain error with operation context. */
    [[nodiscard]] Error PersistenceError(const ErrorCodeDescriptor &descriptor, std::string message);

    /** @brief Reads a bounded durable file into memory. */
    [[nodiscard]] Result<std::string> ReadBoundedFile(const std::filesystem::path &absolutePath, std::uintmax_t maximumBytes);

    /** @brief Reports whether a path is safe for project-relative scene metadata. */
    [[nodiscard]] bool IsSafeProjectRelativePath(const std::filesystem::path &path);

    /** @brief Reports lexical containment of one path beneath another. */
    [[nodiscard]] bool IsContainedBy(const std::filesystem::path &absoluteRoot, const std::filesystem::path &absoluteCandidate);

    /** @brief Reports filesystem-resolved containment beneath a project root. */
    [[nodiscard]] bool IsResolvedContainedBy(const std::filesystem::path &absoluteRoot, const std::filesystem::path &absoluteCandidate);

    [[nodiscard]] Json Vec2Json(Math::Vec2 value);
    [[nodiscard]] Json Vec3Json(Math::Vec3 value);
    [[nodiscard]] Json QuaternionJson(Math::Quaternion value);
    [[nodiscard]] Json TransformJson(const Math::Transform &value);

    [[nodiscard]] Result<Math::Vec2> ParseVec2(const Json &value);
    [[nodiscard]] Result<Math::Vec3> ParseVec3(const Json &value);
    [[nodiscard]] Result<Math::Quaternion> ParseQuaternion(const Json &value);
    [[nodiscard]] Result<Math::Transform> ParseTransform(const Json &value);

    [[nodiscard]] bool HasFields(const Json &value, std::initializer_list<std::string_view> fields);
    [[nodiscard]] bool HasUnsignedFields(const Json &value, std::initializer_list<std::string_view> fields);
    [[nodiscard]] bool AllSucceeded(std::initializer_list<bool> results);

    [[nodiscard]] Result<PrimitiveMeshDescriptor> ParsePrimitive(const Json &value);
    [[nodiscard]] Json PrimitiveJson(const PrimitiveMeshDescriptor &descriptor);

    [[nodiscard]] Json BehaviorFieldValueJson(const Gameplay::BehaviorFieldValue &value);
    [[nodiscard]] Result<Gameplay::BehaviorFieldValue> ParseBehaviorFieldValue(const Json &value);
    [[nodiscard]] Result<Audio::AudioSoundReference> ParseAudioSoundReference(const Json &value);

    [[nodiscard]] Result<Runtime::NavigationSurfaceComponent> ParseNavigationSurface(const Json &value);
    [[nodiscard]] Result<Runtime::NavigationRegionComponent> ParseNavigationRegion(const Json &value);
    [[nodiscard]] Result<Runtime::NavigationModifierComponent> ParseNavigationModifier(const Json &value);
    [[nodiscard]] Result<Runtime::NavigationLinkComponent> ParseNavigationLink(const Json &value);
    [[nodiscard]] Result<Runtime::NavigationAgentComponent> ParseNavigationAgent(const Json &value);
    [[nodiscard]] Result<AI::AiAgentComponent> ParseAiAgent(const Json &value);
    [[nodiscard]] Result<AI::AiControllerComponent> ParseAiController(const Json &value);

    [[nodiscard]] Result<Runtime::RigidBodyComponent> ParseRigidBody(const Json &value);
    [[nodiscard]] Result<std::vector<Runtime::ColliderComponent>> ParseColliders(const Json &value);
    [[nodiscard]] Result<std::vector<Runtime::PhysicsConstraintComponent>> ParsePhysicsConstraints(const Json &value);
    [[nodiscard]] Result<std::vector<Gameplay::BehaviorComponent>> ParseBehaviors(const Json &value);

    [[nodiscard]] Json ComponentsJson(const SceneObjectComponentSet &components);
    void AppendPhysicsComponents(Json &value, const SceneObjectComponentSet &components);
    [[nodiscard]] Result<SceneObjectComponentSet> ParseComponents(const Json &value);

    /** @brief Intermediate scene payload shared by the loader and recovery reader. */
    struct ParsedScene final {
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
    };

    [[nodiscard]] Json SceneJson(const SceneDocumentSnapshot &snapshot);
    [[nodiscard]] Result<ParsedScene> ParseScene(const std::string &contents);

    [[nodiscard]] std::vector<std::byte> Bytes(std::string_view value);
    [[nodiscard]] std::filesystem::path RecoveryPath(const std::filesystem::path &absoluteProjectRoot);
    [[nodiscard]] std::string SceneChecksum(const Json &scene);
    [[nodiscard]] SceneFileFingerprint Fingerprint(std::string_view bytes);
}  // namespace Horo::Editor::ScenePersistenceDetail
