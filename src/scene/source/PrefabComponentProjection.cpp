#include "Horo/Scene/PrefabComponentProjection.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace Horo::SceneSource {
    namespace {
        using Json = nlohmann::json;
        using Kind = Runtime::BuiltinSceneComponentKind;

        /** @brief Names only actual canonical Scene codec fields; unavailable representations stay unsupported. */
        std::string_view Field(const Kind kind) noexcept {
            switch (kind) {
                case Kind::Camera:
                    return "camera";
                case Kind::Light:
                    return "light";
                case Kind::AudioSource:
                    return "audioSource";
                case Kind::NavigationSurface:
                    return "navigationSurface";
                case Kind::NavigationRegion:
                    return "navigationRegion";
                case Kind::NavigationModifier:
                    return "navigationModifier";
                case Kind::NavigationLink:
                    return "navigationLink";
                case Kind::NavigationAgent:
                    return "navigationAgent";
                case Kind::AiAgent:
                    return "aiAgent";
                case Kind::AiController:
                    return "aiController";
                case Kind::CharacterController:
                    return "characterController";
                case Kind::RigidBody:
                    return "rigidBody";
                case Kind::Collider:
                    return "colliders";
                case Kind::PhysicsConstraint:
                    return "physicsConstraints";
                case Kind::PrimitiveMesh:
                case Kind::AudioListener:
                case Kind::UiCanvas:
                case Kind::Count:
                    return {};
            }
            return {};
        }

        /** @brief Copies bounded encoded bytes into the parser's owned character input. */
        std::string PayloadBytes(const std::span<const std::byte> payload) {
            std::string bytes;
            bytes.reserve(payload.size());
            for (const auto byte : payload)
                bytes.push_back(static_cast<char>(byte));
            return bytes;
        }

        /** @brief Rejects duplicate singleton occurrences before moving any candidate field. */
        template <typename T> Result<void> Adopt(std::optional<T> &destination, std::optional<T> &&source) {
            if (!source || destination)
                return Result<void>::Failure(
                    MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered, "Built-in prefab component is absent or duplicated."));
            destination = std::move(source);
            return Result<void>::Success();
        }

        /** @brief Appends one complete collection occurrence without admitting duplicate typed IDs. */
        template <typename T> Result<void> Append(std::vector<T> &destination, std::vector<T> &&source) {
            if (source.size() != 1 || std::ranges::any_of(destination, [&](const auto &existing) {
                return existing.id == source.front().id;
            }))
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered,
                                                       "Built-in prefab collection occurrence is absent or duplicated."));
            destination.push_back(std::move(source.front()));
            return Result<void>::Success();
        }

        /** @brief Moves only the decoded field, preserving all unrelated candidate components. */
        Result<void> Merge(const Kind kind, Runtime::RuntimeComponentSet &destination, SceneObjectComponentSet &&source) {
            switch (kind) {
                case Kind::Camera:
                    return Adopt(destination.camera, std::move(source.camera));
                case Kind::Light:
                    return Adopt(destination.light, std::move(source.light));
                case Kind::AudioSource:
                    return Adopt(destination.audioSource, std::move(source.audioSource));
                case Kind::NavigationSurface:
                    return Adopt(destination.navigationSurface, std::move(source.navigationSurface));
                case Kind::NavigationRegion:
                    return Adopt(destination.navigationRegion, std::move(source.navigationRegion));
                case Kind::NavigationModifier:
                    return Adopt(destination.navigationModifier, std::move(source.navigationModifier));
                case Kind::NavigationLink:
                    return Adopt(destination.navigationLink, std::move(source.navigationLink));
                case Kind::NavigationAgent:
                    return Adopt(destination.navigationAgent, std::move(source.navigationAgent));
                case Kind::AiAgent:
                    return Adopt(destination.aiAgent, std::move(source.aiAgent));
                case Kind::AiController:
                    return Adopt(destination.aiController, std::move(source.aiController));
                case Kind::CharacterController:
                    return Adopt(destination.characterController, std::move(source.characterController));
                case Kind::RigidBody:
                    return Adopt(destination.rigidBody, std::move(source.rigidBody));
                case Kind::Collider:
                    return Append(destination.colliders, std::move(source.colliders));
                case Kind::PhysicsConstraint:
                    return Append(destination.physicsConstraints, std::move(source.physicsConstraints));
                case Kind::PrimitiveMesh:
                case Kind::AudioListener:
                case Kind::UiCanvas:
                case Kind::Count:
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            }
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
        }

        /** @brief Reads bounded canonical payload bytes through the shared duplicate/depth guard. */
        Result<Json> ReadPayload(const Prefab::RawComponentPayload &source) {
            if (source.instance.Value() == 0 || source.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson ||
                source.component.schemaVersion != 1 || Gameplay::ValidateSerializedComponent(source.component).HasError())
                return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
            Foundation::JsonParseGuard guard{64};
            auto payload = Json::parse(PayloadBytes(source.component.payload), std::ref(guard), false);
            if (payload.is_discarded() || !payload.is_object() || guard.HasDuplicate() || guard.IsTooDeep())
                return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
            return Result<Json>::Success(std::move(payload));
        }

        /** @brief Invokes the existing typed component codec once for one exact built-in occurrence. */
        Result<SceneObjectComponentSet> Decode(const Prefab::RawComponentPayload &source, const Kind kind, const std::string_view field) {
            auto payload = ReadPayload(source);
            if (payload.HasError())
                return Result<SceneObjectComponentSet>::Failure(payload.ErrorValue());
            const bool collection = kind == Kind::Collider || kind == Kind::PhysicsConstraint;
            const Json value{{std::string(field), collection ? Json::array({payload.Value()}) : payload.Value()}};
            std::size_t recognized{};
            auto decoded = Detail::ParseComponents(value, recognized);
            if (decoded.HasError())
                return Result<SceneObjectComponentSet>::Failure(WrapError(Prefab::PrefabErrors::CorruptedPayload, decoded.ErrorValue()));
            if (recognized != 1)
                return Result<SceneObjectComponentSet>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            return decoded;
        }

        /** @brief Admits exact canonical primitive geometry without selecting or allocating a renderer. */
        Result<bool> ProjectPrimitive(const Prefab::RawComponentPayload &source,
                                      std::optional<Runtime::PrimitiveMeshDescriptor> &destination) {
            if (destination)
                return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            auto payload = ReadPayload(source);
            if (payload.HasError())
                return Result<bool>::Failure(payload.ErrorValue());
            auto primitive = Detail::ParsePrimitive(payload.Value());
            if (primitive.HasError())
                return Result<bool>::Failure(WrapError(Prefab::PrefabErrors::CorruptedPayload, primitive.ErrorValue()));
            destination = std::move(primitive).Value();
            return Result<bool>::Success(true);
        }
    }  // namespace

    /** @copydoc ProjectPrefabBuiltinComponent */
    Result<bool> ProjectPrefabBuiltinComponent(const Prefab::RawComponentPayload &source, Runtime::RuntimeComponentSet &destination,
                                               std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh) {
        const auto kind = Runtime::FindBuiltinSceneComponent(source.component.typeId.Value());
        if (!kind)
            return Result<bool>::Success(false);
        try {
            if (*kind == Kind::PrimitiveMesh)
                return ProjectPrimitive(source, primitiveMesh);
            const auto field = Field(*kind);
            if (field.empty())
                return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered,
                                                       "This built-in has no supported canonical cooked component representation."));
            auto decoded = Decode(source, *kind, field);
            if (decoded.HasError())
                return Result<bool>::Failure(decoded.ErrorValue());
            auto merged = Merge(*kind, destination, std::move(decoded).Value());
            if (merged.HasError())
                return Result<bool>::Failure(merged.ErrorValue());
            return Result<bool>::Success(true);
        } catch (const nlohmann::json::exception &) {
            return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
        }
    }
}  // namespace Horo::SceneSource
