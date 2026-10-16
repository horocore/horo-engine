#pragma once

#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"

#include <concepts>
#include <tuple>
#include <type_traits>

namespace Horo::Runtime::CellCookDetail {
    // Field order is the version-2 semantic key and Scene provider wire contract.
    // Const and mutable tuples share this single field mapping. Tuples borrow the source;
    // no padding, pointers, or allocations enter the stream. Empty markers are
    // represented by their enclosing variant tag and contribute no payload bytes.
    inline auto ContentFields(std::monostate) {
        return std::tuple{};
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Sha256Digest>
    inline auto ContentFields(T &value) {
        return std::tie(value.bytes);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Math::Vec2>
    inline auto ContentFields(T &v) {
        return std::tie(v.x, v.y);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Math::Vec3>
    inline auto ContentFields(T &v) {
        return std::tie(v.x, v.y, v.z);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Math::Quaternion>
    inline auto ContentFields(T &v) {
        return std::tie(v.x, v.y, v.z, v.w);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Math::Transform>
    inline auto ContentFields(T &v) {
        return std::tie(v.translation, v.rotation, v.scale);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, WorldStreaming::StreamingCellId>
    inline auto ContentFields(T &v) {
        return std::tie(v.x, v.y, v.z, v.lod, v.layer);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, SceneCellPayloadIdentity>
    inline auto ContentFields(T &v) {
        return std::tie(v.partition, v.cell, v.scene, v.revision);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, CameraComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.projection, v.verticalFieldOfViewRadians, v.orthographicHeight, v.nearPlane, v.farPlane, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, LightComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.kind, v.color, v.intensity, v.range, v.innerConeRadians, v.outerConeRadians, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Audio::AudioSoundDefinitionSchemaVersion>
    inline auto ContentFields(T &v) {
        return std::tie(v.major, v.minor);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Audio::AudioSoundExtensionReference>
    inline auto ContentFields(T &v) {
        return std::tie(v.contribution, v.definition, v.contractVersion);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Audio::AudioSoundReference>
    inline auto ContentFields(T &v) {
        return std::tie(v.kind, v.target);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Audio::AudioConcurrencyPolicy>
    inline auto ContentFields(T &v) {
        return std::tie(v.group, v.maxInstances, v.mode);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Audio::AudioSoundPlaybackDefaults>
    inline auto ContentFields(T &v) {
        return std::tie(v.gain, v.pitch, v.bus, v.loop, v.spatialMode, v.enableDoppler, v.playOnStart, v.priority, v.concurrency);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AudioSourceComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.sound, v.playback, v.sceneLifecycle, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AudioListenerComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.view, v.priority, v.weight, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Ui::UiCanvasAssetReference>
    inline auto ContentFields(T &v) {
        return std::tie(v.asset, v.document, v.canvas, v.minimumRevision);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, UiCanvasComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.canvas);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationLocalBounds>
    inline auto ContentFields(T &v) {
        return std::tie(v.center, v.halfExtents);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationSurfaceComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.definition, v.schemaVersion, v.generation, v.bakeScope, v.localBounds, v.profiles, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationRegionComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.surface, v.schemaVersion, v.generation, v.localBounds, v.sourceSelection, v.mode, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationCylinderVolume>
    inline auto ContentFields(T &v) {
        return std::tie(v.center, v.radius, v.halfHeight);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationModifierComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.surface, v.schemaVersion, v.generation, v.volume, v.operation, v.area, v.traversalCost, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationLinkEndpoint>
    inline auto ContentFields(T &v) {
        return std::tie(v.surface, v.localPosition, v.connectionRadiusMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationLinkComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.schemaVersion, v.generation, v.start, v.end, v.kind, v.direction, v.profiles, v.traversalCost, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, NavigationAgentComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.schemaVersion, v.profile, v.filter, v.radiusOverride, v.movementCapability, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AI::AiCapabilitySet>
    inline auto ContentFields(T &v) {
        return std::tie(v.bits);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AI::AiAgentComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.agent, v.schemaVersion, v.startupPolicy, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AI::AiControllerComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.controller, v.decisionAsset, v.blackboardSchema, v.decisionKind, v.requiredCapabilities, v.schemaVersion,
                        v.startupPolicy, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AuthoredPhysicsPose>
    inline auto ContentFields(T &v) {
        return std::tie(v.translation, v.rotation);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsBodyReference>
    inline auto ContentFields(T &v) {
        return std::tie(v.object, v.body);
    }

    inline auto ContentFields(const AuthoredPhysicsNoMass &) {
        return std::tuple{};
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AuthoredPhysicsMass>
    inline auto ContentFields(T &v) {
        return std::tie(v.kilograms);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, AuthoredPhysicsDensity>
    inline auto ContentFields(T &v) {
        return std::tie(v.kilogramsPerCubicMeter);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, RigidBodyComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.body, v.schemaVersion, v.generation, v.motion, v.mass, v.initialLinearVelocity, v.initialAngularVelocity,
                        v.linearDampingPerSecond, v.angularDampingPerSecond, v.maximumLinearSpeed, v.maximumAngularSpeed, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsBoxCollider>
    inline auto ContentFields(T &v) {
        return std::tie(v.halfExtentsMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsSphereCollider>
    inline auto ContentFields(T &v) {
        return std::tie(v.radiusMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsCapsuleCollider>
    inline auto ContentFields(T &v) {
        return std::tie(v.radiusMeters, v.cylindricalHalfHeightMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsStaticPlaneCollider>
    inline auto ContentFields(T &v) {
        return std::tie(v.normal, v.signedDistanceMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsShapeAssetReference>
    inline auto ContentFields(T &v) {
        return std::tie(v.asset, v.subresource);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsColliderMaterialBinding>
    inline auto ContentFields(T &v) {
        return std::tie(v.slot, v.material);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, ColliderComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.collider, v.schemaVersion, v.generation, v.body, v.source, v.localPose, v.scale, v.collisionProfile,
                        v.materials, v.sensor, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsConstraintBodyEndpoint>
    inline auto ContentFields(T &v) {
        return std::tie(v.body, v.localFrame);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsConstraintWorldEndpoint>
    inline auto ContentFields(T &v) {
        return std::tie(v.frame);
    }

    inline auto ContentFields(const PhysicsFixedConstraint &) {
        return std::tuple{};
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsDistanceConstraint>
    inline auto ContentFields(T &v) {
        return std::tie(v.minimumMeters, v.maximumMeters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PhysicsConstraintComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.constraint, v.schemaVersion, v.generation, v.first, v.second, v.parameters, v.enabled);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Gameplay::BehaviorField>
    inline auto ContentFields(T &v) {
        return std::tie(v.name, v.value);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Gameplay::BehaviorComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.instanceId, v.typeId, v.schemaVersion, v.enabled, v.fields);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, Gameplay::SerializedComponent>
    inline auto ContentFields(T &v) {
        return std::tie(v.typeId, v.schemaVersion, v.encoding, v.payload);
    }

    inline auto ContentFields(BoxMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const BoxMeshParameters &v) {
        return std::tie(v.size);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, SphereMeshParameters>
    inline auto ContentFields(T &v) {
        return std::tie(v.radius, v.slices, v.stacks);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, CapsuleMeshParameters>
    inline auto ContentFields(T &v) {
        return std::tie(v.radius, v.totalHeight, v.radialSegments, v.hemisphereRings);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, CylinderMeshParameters>
    inline auto ContentFields(T &v) {
        return std::tie(v.radius, v.height, v.radialSegments);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, ConeMeshParameters>
    inline auto ContentFields(T &v) {
        return std::tie(v.radius, v.height, v.radialSegments);
    }

    inline auto ContentFields(PlaneMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const PlaneMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(QuadMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const QuadMeshParameters &v) {
        return std::tie(v.size);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, PrimitiveMeshDescriptor>
    inline auto ContentFields(T &v) {
        return std::tie(v.type, v.version, v.parameters);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, RuntimeComponentSet>
    inline auto ContentFields(T &v) {
        return std::tie(v.camera, v.light, v.audioSource, v.audioListener, v.uiCanvas, v.navigationSurface, v.navigationRegion,
                        v.navigationModifier, v.navigationLink, v.navigationAgent, v.aiAgent, v.aiController, v.rigidBody, v.colliders,
                        v.physicsConstraints, v.behaviors, v.gameplayComponents);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, RuntimeEntityDefinition>
    inline auto ContentFields(T &v) {
        return std::tie(v.object, v.parent, v.localTransform, v.primitiveMesh, v.components);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, SceneAssetDependency>
    inline auto ContentFields(T &v) {
        return std::tie(v.id, v.expectedType);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, SceneCellComponentSchema>
    inline auto ContentFields(T &v) {
        return std::tie(v.type, v.version);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, SceneCellBehaviorSchema>
    inline auto ContentFields(T &v) {
        return std::tie(v.type, v.version);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, TriggerVolumeProjectBinding>
    inline auto ContentFields(T &v) {
        return std::tie(v.collisionSchemaFingerprint, v.profile, v.material, v.materialSourceRevision, v.materialPayloadDigest);
    }

    template <class T>
        requires std::same_as<std::remove_cv_t<T>, TriggerVolumeMigrationEvidence>
    inline auto ContentFields(T &v) {
        return std::tie(v.object, v.body, v.collider, v.binding);
    }
}  // namespace Horo::Runtime::CellCookDetail
