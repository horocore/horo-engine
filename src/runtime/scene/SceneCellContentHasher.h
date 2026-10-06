#pragma once

#include "Horo/Runtime/Scene/IncrementalSceneCellCook.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace Horo::Runtime::CellCookDetail {
    /** @brief Streams a versioned semantic key without padding, pointers, native endian data or hash-time allocation. */
    class ContentHasher final {
    public:
        ContentHasher(const CancellationToken &cancellation, std::size_t maximumBytes)
            : cancellation_(cancellation), remaining_(maximumBytes) {}

        template <class... T> void Fields(const T &...values) {
            (Add(values), ...);
        }

        template <class T> void Add(const T &value) {
            if constexpr (std::is_integral_v<T>) {
                const auto bits = static_cast<std::uint64_t>(value);
                std::array<std::byte, 8> bytes{};
                for (unsigned i = 0; i < 8; ++i)
                    bytes[i] = static_cast<std::byte>(bits >> (8U * i));
                Bytes(bytes);
            } else if constexpr (std::is_enum_v<T>) {
                Add(static_cast<std::underlying_type_t<T>>(value));
            } else if constexpr (std::is_same_v<T, float>) {
                Add(std::bit_cast<std::uint32_t>(value));
            } else if constexpr (std::is_same_v<T, double>) {
                Add(std::bit_cast<std::uint64_t>(value));
            } else if constexpr (requires { value.Bytes(); }) {
                Add(value.Bytes());
            } else if constexpr (requires { value.Asset(); }) {
                Add(value.Asset());
            } else if constexpr (requires { value.Value(); }) {
                Add(value.Value());
            } else {
                Add(value.value);
            }
        }

        void Add(const std::string &value) {
            Add(std::string_view{value});
        }

        void Add(std::string_view value) {
            Add(value.size());
            Bytes(std::as_bytes(std::span{value.data(), value.size()}));
        }

        void Add(std::monostate) {}

        template <class T> void Add(const std::optional<T> &value) {
            Add(value.has_value());
            if (value)
                Add(*value);
        }

        template <class... T> void Add(const std::variant<T...> &value) {
            Add(value.index());
            std::visit([this](const auto &item) {
                Add(item);
            }, value);
        }

        template <class T> void Add(const std::vector<T> &values) {
            Add(std::span<const T>{values});
        }

        template <class T, std::size_t N> void Add(const std::array<T, N> &values) {
            Add(std::span<const T>{values});
        }

        template <class T> void Add(std::span<const T> values) {
            Add(values.size());
            if constexpr (std::is_same_v<T, std::byte> || std::is_same_v<T, std::uint8_t>) {
                Bytes(std::as_bytes(values));
            } else {
                if (values.size() > remaining_ / 8) {
                    failed_ = true;
                    return;
                }
                for (const auto &value : values) {
                    if (failed_ || cancellation_.IsCancellationRequested())
                        break;
                    Add(value);
                }
            }
        }

        void Add(const Sha256Digest &value) {
            Add(value.bytes);
        }

        void Add(const Math::Vec2 &v) {
            Fields(v.x, v.y);
        }

        void Add(const Math::Vec3 &v) {
            Fields(v.x, v.y, v.z);
        }

        void Add(const Math::Quaternion &v) {
            Fields(v.x, v.y, v.z, v.w);
        }

        void Add(const Math::Transform &v) {
            Fields(v.translation, v.rotation, v.scale);
        }

        void Add(const WorldStreaming::StreamingCellId &v) {
            Fields(v.x, v.y, v.z, v.lod, v.layer);
        }

        void Add(const SceneCellPayloadIdentity &v) {
            Fields(v.partition, v.cell, v.scene, v.revision);
        }

        void Add(const CameraComponent &v) {
            Fields(v.projection, v.verticalFieldOfViewRadians, v.orthographicHeight, v.nearPlane, v.farPlane, v.enabled);
        }

        void Add(const LightComponent &v) {
            Fields(v.kind, v.color, v.intensity, v.range, v.innerConeRadians, v.outerConeRadians, v.enabled);
        }

        void Add(const Audio::AudioSoundDefinitionSchemaVersion &v) {
            Fields(v.major, v.minor);
        }

        void Add(const Audio::AudioSoundExtensionReference &v) {
            Fields(v.contribution, v.definition, v.contractVersion);
        }

        void Add(const Audio::AudioSoundReference &v) {
            Fields(v.kind, v.target);
        }

        void Add(const Audio::AudioConcurrencyPolicy &v) {
            Fields(v.group, v.maxInstances, v.mode);
        }

        void Add(const Audio::AudioSoundPlaybackDefaults &v) {
            Fields(v.gain, v.pitch, v.bus, v.loop, v.spatialMode, v.enableDoppler, v.playOnStart, v.priority, v.concurrency);
        }

        void Add(const AudioSourceComponent &v) {
            Fields(v.sound, v.playback, v.sceneLifecycle, v.enabled);
        }

        void Add(const AudioListenerComponent &v) {
            Fields(v.view, v.priority, v.weight, v.enabled);
        }

        void Add(const Ui::UiCanvasAssetReference &v) {
            Fields(v.asset, v.document, v.canvas, v.minimumRevision);
        }

        void Add(const UiCanvasComponent &v) {
            Fields(v.canvas);
        }

        void Add(const NavigationLocalBounds &v) {
            Fields(v.center, v.halfExtents);
        }

        void Add(const NavigationSurfaceComponent &v) {
            Fields(v.id, v.definition, v.schemaVersion, v.generation, v.bakeScope, v.localBounds, v.profiles, v.enabled);
        }

        void Add(const NavigationRegionComponent &v) {
            Fields(v.id, v.surface, v.schemaVersion, v.generation, v.localBounds, v.sourceSelection, v.mode, v.enabled);
        }

        void Add(const NavigationCylinderVolume &v) {
            Fields(v.center, v.radius, v.halfHeight);
        }

        void Add(const NavigationModifierComponent &v) {
            Fields(v.id, v.surface, v.schemaVersion, v.generation, v.volume, v.operation, v.area, v.traversalCost, v.enabled);
        }

        void Add(const NavigationLinkEndpoint &v) {
            Fields(v.surface, v.localPosition, v.connectionRadiusMeters);
        }

        void Add(const NavigationLinkComponent &v) {
            Fields(v.id, v.schemaVersion, v.generation, v.start, v.end, v.kind, v.direction, v.profiles, v.traversalCost, v.enabled);
        }

        void Add(const NavigationAgentComponent &v) {
            Fields(v.schemaVersion, v.profile, v.filter, v.radiusOverride, v.movementCapability, v.enabled);
        }

        void Add(const AI::AiCapabilitySet &v) {
            Fields(v.bits);
        }

        void Add(const AI::AiAgentComponent &v) {
            Fields(v.agent, v.schemaVersion, v.startupPolicy, v.enabled);
        }

        void Add(const AI::AiControllerComponent &v) {
            Fields(v.controller, v.decisionAsset, v.blackboardSchema, v.decisionKind, v.requiredCapabilities, v.schemaVersion,
                   v.startupPolicy, v.enabled);
        }

        void Add(const AuthoredPhysicsPose &v) {
            Fields(v.translation, v.rotation);
        }

        void Add(const PhysicsBodyReference &v) {
            Fields(v.object, v.body);
        }

        void Add(const AuthoredPhysicsNoMass &v) {
            (void)v;
        }

        void Add(const AuthoredPhysicsMass &v) {
            Fields(v.kilograms);
        }

        void Add(const AuthoredPhysicsDensity &v) {
            Fields(v.kilogramsPerCubicMeter);
        }

        void Add(const RigidBodyComponent &v) {
            Fields(v.id, v.body, v.schemaVersion, v.generation, v.motion, v.mass, v.initialLinearVelocity, v.initialAngularVelocity,
                   v.linearDampingPerSecond, v.angularDampingPerSecond, v.maximumLinearSpeed, v.maximumAngularSpeed, v.enabled);
        }

        void Add(const PhysicsBoxCollider &v) {
            Fields(v.halfExtentsMeters);
        }

        void Add(const PhysicsSphereCollider &v) {
            Fields(v.radiusMeters);
        }

        void Add(const PhysicsCapsuleCollider &v) {
            Fields(v.radiusMeters, v.cylindricalHalfHeightMeters);
        }

        void Add(const PhysicsStaticPlaneCollider &v) {
            Fields(v.normal, v.signedDistanceMeters);
        }

        void Add(const PhysicsShapeAssetReference &v) {
            Fields(v.asset, v.subresource);
        }

        void Add(const PhysicsColliderMaterialBinding &v) {
            Fields(v.slot, v.material);
        }

        void Add(const ColliderComponent &v) {
            Fields(v.id, v.collider, v.schemaVersion, v.generation, v.body, v.source, v.localPose, v.scale, v.collisionProfile, v.materials,
                   v.sensor, v.enabled);
        }

        void Add(const PhysicsConstraintBodyEndpoint &v) {
            Fields(v.body, v.localFrame);
        }

        void Add(const PhysicsConstraintWorldEndpoint &v) {
            Fields(v.frame);
        }

        void Add(const PhysicsFixedConstraint &v) {
            (void)v;
        }

        void Add(const PhysicsDistanceConstraint &v) {
            Fields(v.minimumMeters, v.maximumMeters);
        }

        void Add(const PhysicsConstraintComponent &v) {
            Fields(v.id, v.constraint, v.schemaVersion, v.generation, v.first, v.second, v.parameters, v.enabled);
        }

        void Add(const Gameplay::BehaviorField &v) {
            Fields(v.name, v.value);
        }

        void Add(const Gameplay::BehaviorComponent &v) {
            Fields(v.instanceId, v.typeId, v.schemaVersion, v.enabled, v.fields);
        }

        void Add(const Gameplay::SerializedComponent &v) {
            Fields(v.typeId, v.schemaVersion, v.encoding, v.payload);
        }

        void Add(const BoxMeshParameters &v) {
            Fields(v.size);
        }

        void Add(const SphereMeshParameters &v) {
            Fields(v.radius, v.slices, v.stacks);
        }

        void Add(const CapsuleMeshParameters &v) {
            Fields(v.radius, v.totalHeight, v.radialSegments, v.hemisphereRings);
        }

        void Add(const CylinderMeshParameters &v) {
            Fields(v.radius, v.height, v.radialSegments);
        }

        void Add(const ConeMeshParameters &v) {
            Fields(v.radius, v.height, v.radialSegments);
        }

        void Add(const PlaneMeshParameters &v) {
            Fields(v.size);
        }

        void Add(const QuadMeshParameters &v) {
            Fields(v.size);
        }

        void Add(const PrimitiveMeshDescriptor &v) {
            Fields(v.type, v.version, v.parameters);
        }

        void Add(const RuntimeComponentSet &v) {
            Fields(v.camera, v.light, v.audioSource, v.audioListener, v.uiCanvas, v.navigationSurface, v.navigationRegion,
                   v.navigationModifier, v.navigationLink, v.navigationAgent, v.aiAgent, v.aiController, v.rigidBody, v.colliders,
                   v.physicsConstraints, v.behaviors, v.gameplayComponents);
        }

        void Add(const RuntimeEntityDefinition &v) {
            Fields(v.object, v.parent, v.localTransform, v.primitiveMesh, v.components);
        }

        void Add(const SceneAssetDependency &v) {
            Fields(v.id, v.expectedType);
        }

        void Add(const SceneCellComponentSchema &v) {
            Fields(v.type, v.version);
        }

        void Add(const SceneCellBehaviorSchema &v) {
            Fields(v.type, v.version);
        }

        [[nodiscard]] Result<Sha256Digest> Finish() {
            if (cancellation_.IsCancellationRequested())
                return Result<Sha256Digest>::Failure(MakeError(SceneCellPayloadErrors::Cancelled));
            if (failed_)
                return Result<Sha256Digest>::Failure(MakeError(SceneCellPayloadErrors::CapacityExceeded));
            return Result<Sha256Digest>::Success(hash_.Finalize());
        }

    private:
        /** @brief Bounds total key bytes and observes cancellation on every fragment before SHA work. */
        void Bytes(std::span<const std::byte> bytes) {
            if (failed_ || cancellation_.IsCancellationRequested())
                return;
            if (bytes.size() > remaining_) {
                failed_ = true;
                return;
            }
            remaining_ -= bytes.size();
            while (!bytes.empty() && !cancellation_.IsCancellationRequested()) {
                const auto count = std::min(bytes.size(), std::size_t{65536});
                if (!hash_.Update(bytes.first(count))) {
                    failed_ = true;
                    return;
                }
                bytes = bytes.subspan(count);
            }
        }

        const CancellationToken &cancellation_;
        std::size_t remaining_;
        bool failed_{};
        Sha256Builder hash_;
    };
}  // namespace Horo::Runtime::CellCookDetail
