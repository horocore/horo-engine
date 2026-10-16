#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <vector>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Requires an exact declared material lease before decoding its immutable canonical payload. */
        [[nodiscard]] Result<Runtime::RuntimeSceneAssetView> ResolveCharacterAsset(const Runtime::RuntimeSceneDefinition &definition,
                                                                                   Runtime::RuntimeSceneView scene,
                                                                                   Assets::AssetId material) {
            const auto dependency = std::ranges::find(definition.AssetDependencies(), material, &Runtime::SceneAssetDependency::id);
            const auto resolved = scene.FindAsset(material);
            if (dependency == definition.AssetDependencies().end() || !resolved || !resolved->type ||
                *resolved->type != dependency->expectedType)
                return Result<Runtime::RuntimeSceneAssetView>::Failure(
                    MakeError(PhysicsErrors::MaterialDescriptorInvalid,
                              "Character material is not retained by the exact candidate Scene dependency closure."));
            return Result<Runtime::RuntimeSceneAssetView>::Success(*resolved);
        }

        /** @brief Rejects native descendants that would retain pre-recovery poses; visual hierarchy remains unrestricted. */
        [[nodiscard]] Result<void> RequireCharacterTopology(const Runtime::RuntimeSceneView scene) {
            for (std::size_t index = 0; index < scene.SlotCount(); ++index) {
                const auto entity = scene.EntityAt(index);
                if (!entity)
                    continue;
                const auto &components = *entity->components;
                const bool character = components.characterController && components.characterController->enabled;
                const bool native = (components.rigidBody && components.rigidBody->enabled) ||
                                    std::ranges::any_of(components.colliders, [](const auto &collider) {
                    return collider.enabled;
                });
                if (!character && !native)
                    continue;
                auto ancestor = entity;
                std::size_t depth{};
                while (ancestor) {
                    const auto &controller = ancestor->components->characterController;
                    if (controller && controller->enabled && (native || ancestor->entity != entity->entity))
                        return Result<void>::Failure(
                            AddActivationContext(MakeError(PhysicsErrors::OperationUnsupported,
                                                           "Enabled native contributors and nested controllers below a Character root "
                                                           "require unsupported dependent restaging."),
                                                 "character topology", ancestor->authoredObject.value_or(Runtime::SceneObjectId{}),
                                                 controller->id.value, std::nullopt));
                    if (!ancestor->parent)
                        break;
                    auto parent = scene.Get(*ancestor->parent);
                    if (parent.HasError() || ++depth > scene.SlotCount())
                        return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Character parent hierarchy is invalid."));
                    ancestor = parent.Value();
                }
            }
            return Result<void>::Success();
        }

        /** @brief Resolves canonical immutable material bytes and uses their actual source revision as captured evidence. */
        [[nodiscard]] Result<PhysicsQueryMaterial> CharacterMaterial(const Runtime::RuntimeSceneDefinition &definition,
                                                                     Runtime::RuntimeSceneView scene, Runtime::SceneObjectId object,
                                                                     const Runtime::CharacterControllerComponent &component) {
            const auto &binding = *component.collision.defaultMaterial;
            auto resolved = ResolveCharacterAsset(definition, scene, binding.material);
            if (resolved.HasError())
                return Result<PhysicsQueryMaterial>::Failure(resolved.ErrorValue());
            if (!resolved.Value().type || resolved.Value().type->Value() != PhysicsMaterialAssetTypeName ||
                resolved.Value().bytes.size() != PhysicsMaterialPayloadSize)
                return Result<PhysicsQueryMaterial>::Failure(
                    AddActivationContext(MakeError(PhysicsErrors::MaterialDescriptorInvalid,
                                                   "Character material requires the canonical physical-material asset payload."),
                                         "character material", object, component.id.value, binding.material));
            std::array<std::byte, PhysicsMaterialPayloadSize> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>(resolved.Value().bytes[index]);
            auto material = DecodePhysicsMaterialAsset(bytes);
            if (material.HasError())
                return Result<PhysicsQueryMaterial>::Failure(material.ErrorValue());
            if (material.Value().Id().Asset() != binding.material)
                return Result<PhysicsQueryMaterial>::Failure(
                    MakeError(PhysicsErrors::MaterialDescriptorInvalid, "Character material identity mismatch."));
            return Result<PhysicsQueryMaterial>::Success({binding.material, material.Value().SourceRevision(), binding.slot});
        }

        /** @brief Copies durable values into the existing runtime descriptor, retaining no authoring storage. */
        [[nodiscard]] Character::CharacterControllerDescriptor CharacterDescriptor(const Runtime::CharacterControllerComponent &component,
                                                                                   Math::Transform world, PhysicsQueryMaterial material,
                                                                                   std::uint64_t sceneGeneration) {
            const auto &geometry = component.geometry;
            const auto &movement = component.movement;
            Character::CharacterControllerDescriptor result{.sceneGeneration = sceneGeneration,
                                                            .capsule = {geometry.capsule.radiusMeters,
                                                                        geometry.capsule.cylindricalHalfHeightMeters},
                                                            .collisionRootPosition = world.translation,
                                                            .up = geometry.up,
                                                            .gravity = geometry.gravity,
                                                            .collisionProfile = *component.collision.profile,
                                                            .queryChannel = *component.collision.queryChannel,
                                                            .defaultMaterial = material,
                                                            .skinWidthMeters = movement.skinWidthMeters,
                                                            .minimumMoveDistanceMeters = movement.minimumMoveDistanceMeters,
                                                            .maximumStepHeightMeters = movement.maximumStepHeightMeters,
                                                            .maximumSlopeDegrees = movement.maximumSlopeDegrees,
                                                            .preserveHorizontalSpeedOnSlopes = movement.preserveHorizontalSpeed,
                                                            .steepSlopePolicy = movement.steepSlopePolicy,
                                                            .maximumContacts = movement.maximumContacts,
                                                            .selectors = {component.collision.requiredLayer,
                                                                          component.collision.requiredProfile,
                                                                          {}},
                                                            .jumpSpeedMetersPerSecond = movement.jumpSpeedMetersPerSecond,
                                                            .allowDynamicPlatformAttachment = movement.dynamicPlatformSupport,
                                                            .inheritPlatformHeading = movement.inheritPlatformHeading,
                                                            .platformLeaveVelocity = movement.platformLeaveVelocity};
            if (geometry.crouchedCapsule)
                result.crouchedCapsule =
                    PhysicsCapsuleShape{geometry.crouchedCapsule->radiusMeters, geometry.crouchedCapsule->cylindricalHalfHeightMeters};
            return result;
        }
    }  // namespace

    /** @copydoc ValidateCharacterProjectBindings */
    Result<void> ValidateCharacterProjectBindings(const std::span<const PlannedCharacter> characters,
                                                  const PhysicsWorldSimulationBinding &simulation) {
        if (characters.empty())
            return Result<void>::Success();
        if (!simulation.schema || simulation.generation == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable,
                                                   "Enabled Character requires an explicit captured Project collision schema."));
        const auto &schema = *simulation.schema;
        for (const auto &planned : characters) {
            const auto &descriptor = planned.descriptor;
            const auto contextual = [&](Error error) {
                return Result<void>::Failure(AddActivationContext(std::move(error), "character project binding", planned.object,
                                                                  planned.component.value, std::nullopt));
            };
            if (auto query = schema.ResolveQuery(descriptor.collisionProfile, descriptor.queryChannel); query.HasError())
                return contextual(query.ErrorValue());
            const auto channel = std::ranges::find(schema.QueryChannels(), descriptor.queryChannel, &CollisionQueryChannelDefinition::id);
            if (channel == schema.QueryChannels().end() || !channel->enabled)
                return contextual(MakeError(PhysicsErrors::DescriptorInvalid, "Character query channel is absent or disabled."));
            if (descriptor.selectors.requiredLayer) {
                const auto layer = std::ranges::find(schema.Layers(), *descriptor.selectors.requiredLayer, &CollisionLayerDefinition::id);
                if (layer == schema.Layers().end() || !layer->enabled)
                    return contextual(MakeError(PhysicsErrors::DescriptorInvalid, "Character required layer is absent or disabled."));
            }
            if (descriptor.selectors.requiredProfile) {
                if (auto profile = schema.ResolveProfile(*descriptor.selectors.requiredProfile); profile.HasError())
                    return contextual(profile.ErrorValue());
            }
        }
        return Result<void>::Success();
    }

    /** @brief Plans exact controller occurrences using the same cached hierarchy resolver as rigid bodies. */
    [[nodiscard]] Result<std::vector<PlannedCharacter>> BuildPlannedCharacters(const Runtime::RuntimeSceneDefinition &definition,
                                                                               const Runtime::RuntimeSceneView scene) {
        if (auto topology = RequireCharacterTopology(scene); topology.HasError())
            return Result<std::vector<PlannedCharacter>>::Failure(topology.ErrorValue());
        std::vector<PlannedCharacter> result;
        std::vector<std::uint8_t> states(scene.SlotCount());
        std::vector<std::optional<Math::Transform>> cache(scene.SlotCount());
        for (const auto &entity : definition.Entities()) {
            if (!entity.components.characterController || !entity.components.characterController->enabled)
                continue;
            const auto &component = *entity.components.characterController;
            const auto owner = scene.Find(entity.object);
            if (!owner)
                return Result<std::vector<PlannedCharacter>>::Failure(MakeError(PhysicsErrors::HandleStale));
            auto world = ResolveSceneWorldTransform(scene, owner->entity.index, states, cache);
            if (world.HasError())
                return Result<std::vector<PlannedCharacter>>::Failure(world.ErrorValue());
            auto material = CharacterMaterial(definition, scene, entity.object, component);
            if (material.HasError())
                return Result<std::vector<PlannedCharacter>>::Failure(material.ErrorValue());
            result.push_back({entity.object, component.id, *owner,
                              CharacterDescriptor(component, world.Value(), material.Value(), scene.RuntimeId().value),
                              world.Value().rotation, component.collision.excludedBody});
        }
        std::ranges::sort(result, {}, [](const auto &value) {
            return value.object.value;
        });
        return Result<std::vector<PlannedCharacter>>::Success(std::move(result));
    }

}  // namespace Horo::Physics::Detail
