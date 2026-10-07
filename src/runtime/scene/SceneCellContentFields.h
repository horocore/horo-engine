#pragma once

#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"

#include <tuple>

namespace Horo::Runtime::CellCookDetail {
    // Field order is the version-1 semantic key contract. Tuples borrow the source;
    // no padding, pointers, or allocations enter the stream. Empty markers are
    // represented by their enclosing variant tag and contribute no payload bytes.
    inline auto ContentFields(std::monostate) {
        return std::tuple{};
    }

    inline auto ContentFields(const Sha256Digest &value) {
        return std::tie(value.bytes);
    }

    inline auto ContentFields(const Math::Vec2 &v) {
        return std::tie(v.x, v.y);
    }

    inline auto ContentFields(const Math::Vec3 &v) {
        return std::tie(v.x, v.y, v.z);
    }

    inline auto ContentFields(const Math::Quaternion &v) {
        return std::tie(v.x, v.y, v.z, v.w);
    }

    inline auto ContentFields(const Math::Transform &v) {
        return std::tie(v.translation, v.rotation, v.scale);
    }

    inline auto ContentFields(const WorldStreaming::StreamingCellId &v) {
        return std::tie(v.x, v.y, v.z, v.lod, v.layer);
    }

    inline auto ContentFields(const SceneCellPayloadIdentity &v) {
        return std::tie(v.partition, v.cell, v.scene, v.revision);
    }

    inline auto ContentFields(const CameraComponent &v) {
        return std::tie(v.projection, v.verticalFieldOfViewRadians, v.orthographicHeight, v.nearPlane, v.farPlane, v.enabled);
    }

    inline auto ContentFields(const LightComponent &v) {
        return std::tie(v.kind, v.color, v.intensity, v.range, v.innerConeRadians, v.outerConeRadians, v.enabled);
    }

    inline auto ContentFields(const Audio::AudioSoundDefinitionSchemaVersion &v) {
        return std::tie(v.major, v.minor);
    }

    inline auto ContentFields(const Audio::AudioSoundExtensionReference &v) {
        return std::tie(v.contribution, v.definition, v.contractVersion);
    }

    inline auto ContentFields(const Audio::AudioSoundReference &v) {
        return std::tie(v.kind, v.target);
    }

    inline auto ContentFields(const Audio::AudioConcurrencyPolicy &v) {
        return std::tie(v.group, v.maxInstances, v.mode);
    }

    inline auto ContentFields(const Audio::AudioSoundPlaybackDefaults &v) {
        return std::tie(v.gain, v.pitch, v.bus, v.loop, v.spatialMode, v.enableDoppler, v.playOnStart, v.priority, v.concurrency);
    }

    inline auto ContentFields(const AudioSourceComponent &v) {
        return std::tie(v.sound, v.playback, v.sceneLifecycle, v.enabled);
    }

    inline auto ContentFields(const AudioListenerComponent &v) {
        return std::tie(v.view, v.priority, v.weight, v.enabled);
    }

    inline auto ContentFields(const Ui::UiCanvasAssetReference &v) {
        return std::tie(v.asset, v.document, v.canvas, v.minimumRevision);
    }

    inline auto ContentFields(const UiCanvasComponent &v) {
        return std::tie(v.canvas);
    }

    inline auto ContentFields(const NavigationLocalBounds &v) {
        return std::tie(v.center, v.halfExtents);
    }

    inline auto ContentFields(const NavigationSurfaceComponent &v) {
        return std::tie(v.id, v.definition, v.schemaVersion, v.generation, v.bakeScope, v.localBounds, v.profiles, v.enabled);
    }

    inline auto ContentFields(const NavigationRegionComponent &v) {
        return std::tie(v.id, v.surface, v.schemaVersion, v.generation, v.localBounds, v.sourceSelection, v.mode, v.enabled);
    }

    inline auto ContentFields(const NavigationCylinderVolume &v) {
        return std::tie(v.center, v.radius, v.halfHeight);
    }

    inline auto ContentFields(const NavigationModifierComponent &v) {
        return std::tie(v.id, v.surface, v.schemaVersion, v.generation, v.volume, v.operation, v.area, v.traversalCost, v.enabled);
    }

    inline auto ContentFields(const NavigationLinkEndpoint &v) {
        return std::tie(v.surface, v.localPosition, v.connectionRadiusMeters);
    }

    inline auto ContentFields(const NavigationLinkComponent &v) {
        return std::tie(v.id, v.schemaVersion, v.generation, v.start, v.end, v.kind, v.direction, v.profiles, v.traversalCost, v.enabled);
    }

    inline auto ContentFields(const NavigationAgentComponent &v) {
        return std::tie(v.schemaVersion, v.profile, v.filter, v.radiusOverride, v.movementCapability, v.enabled);
    }

    inline auto ContentFields(const AI::AiCapabilitySet &v) {
        return std::tie(v.bits);
    }

    inline auto ContentFields(const AI::AiAgentComponent &v) {
        return std::tie(v.agent, v.schemaVersion, v.startupPolicy, v.enabled);
    }

    inline auto ContentFields(const AI::AiControllerComponent &v) {
        return std::tie(v.controller, v.decisionAsset, v.blackboardSchema, v.decisionKind, v.requiredCapabilities, v.schemaVersion,
                        v.startupPolicy, v.enabled);
    }

    inline auto ContentFields(const AuthoredPhysicsPose &v) {
        return std::tie(v.translation, v.rotation);
    }

    inline auto ContentFields(const PhysicsBodyReference &v) {
        return std::tie(v.object, v.body);
    }

    inline auto ContentFields(const AuthoredPhysicsNoMass &) {
        return std::tuple{};
    }

    inline auto ContentFields(const AuthoredPhysicsMass &v) {
        return std::tie(v.kilograms);
    }

    inline auto ContentFields(const AuthoredPhysicsDensity &v) {
        return std::tie(v.kilogramsPerCubicMeter);
    }

    inline auto ContentFields(const RigidBodyComponent &v) {
        return std::tie(v.id, v.body, v.schemaVersion, v.generation, v.motion, v.mass, v.initialLinearVelocity, v.initialAngularVelocity,
                        v.linearDampingPerSecond, v.angularDampingPerSecond, v.maximumLinearSpeed, v.maximumAngularSpeed, v.enabled);
    }

    inline auto ContentFields(const PhysicsBoxCollider &v) {
        return std::tie(v.halfExtentsMeters);
    }

    inline auto ContentFields(const PhysicsSphereCollider &v) {
        return std::tie(v.radiusMeters);
    }

    inline auto ContentFields(const PhysicsCapsuleCollider &v) {
        return std::tie(v.radiusMeters, v.cylindricalHalfHeightMeters);
    }

    inline auto ContentFields(const PhysicsStaticPlaneCollider &v) {
        return std::tie(v.normal, v.signedDistanceMeters);
    }

    inline auto ContentFields(const PhysicsShapeAssetReference &v) {
        return std::tie(v.asset, v.subresource);
    }

    inline auto ContentFields(const PhysicsColliderMaterialBinding &v) {
        return std::tie(v.slot, v.material);
    }

    inline auto ContentFields(const ColliderComponent &v) {
        return std::tie(v.id, v.collider, v.schemaVersion, v.generation, v.body, v.source, v.localPose, v.scale, v.collisionProfile,
                        v.materials, v.sensor, v.enabled);
    }

    inline auto ContentFields(const PhysicsConstraintBodyEndpoint &v) {
        return std::tie(v.body, v.localFrame);
    }

    inline auto ContentFields(const PhysicsConstraintWorldEndpoint &v) {
        return std::tie(v.frame);
    }

    inline auto ContentFields(const PhysicsFixedConstraint &) {
        return std::tuple{};
    }

    inline auto ContentFields(const PhysicsDistanceConstraint &v) {
        return std::tie(v.minimumMeters, v.maximumMeters);
    }

    inline auto ContentFields(const PhysicsConstraintComponent &v) {
        return std::tie(v.id, v.constraint, v.schemaVersion, v.generation, v.first, v.second, v.parameters, v.enabled);
    }

    inline auto ContentFields(const Gameplay::BehaviorField &v) {
        return std::tie(v.name, v.value);
    }

    inline auto ContentFields(const Gameplay::BehaviorComponent &v) {
        return std::tie(v.instanceId, v.typeId, v.schemaVersion, v.enabled, v.fields);
    }

    inline auto ContentFields(const Gameplay::SerializedComponent &v) {
        return std::tie(v.typeId, v.schemaVersion, v.encoding, v.payload);
    }

    inline auto ContentFields(const BoxMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const SphereMeshParameters &v) {
        return std::tie(v.radius, v.slices, v.stacks);
    }

    inline auto ContentFields(const CapsuleMeshParameters &v) {
        return std::tie(v.radius, v.totalHeight, v.radialSegments, v.hemisphereRings);
    }

    inline auto ContentFields(const CylinderMeshParameters &v) {
        return std::tie(v.radius, v.height, v.radialSegments);
    }

    inline auto ContentFields(const ConeMeshParameters &v) {
        return std::tie(v.radius, v.height, v.radialSegments);
    }

    inline auto ContentFields(const PlaneMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const QuadMeshParameters &v) {
        return std::tie(v.size);
    }

    inline auto ContentFields(const PrimitiveMeshDescriptor &v) {
        return std::tie(v.type, v.version, v.parameters);
    }

    inline auto ContentFields(const RuntimeComponentSet &v) {
        return std::tie(v.camera, v.light, v.audioSource, v.audioListener, v.uiCanvas, v.navigationSurface, v.navigationRegion,
                        v.navigationModifier, v.navigationLink, v.navigationAgent, v.aiAgent, v.aiController, v.rigidBody, v.colliders,
                        v.physicsConstraints, v.behaviors, v.gameplayComponents);
    }

    inline auto ContentFields(const RuntimeEntityDefinition &v) {
        return std::tie(v.object, v.parent, v.localTransform, v.primitiveMesh, v.components);
    }

    inline auto ContentFields(const SceneAssetDependency &v) {
        return std::tie(v.id, v.expectedType);
    }

    inline auto ContentFields(const SceneCellComponentSchema &v) {
        return std::tie(v.type, v.version);
    }

    inline auto ContentFields(const SceneCellBehaviorSchema &v) {
        return std::tie(v.type, v.version);
    }
}  // namespace Horo::Runtime::CellCookDetail
