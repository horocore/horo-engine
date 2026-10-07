#include "SceneCellContentHasher.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        template <class... T> Sha256Digest Encoded(const T &...values) {
            const CancellationToken cancellation;
            CellCookDetail::ContentHasher hash{cancellation, 1024 * 1024};
            hash.Fields(values...);
            const auto result = hash.Finish();
            REQUIRE(result.HasValue());
            return result.Value();
        }

        // The expected member sequence is independent of the production projection
        // dispatch. It protects the version-1 encoding when components evolve.
        template <class T, class... Fields> void CheckFields(const T &value, const Fields &...fields) {
            CHECK(Encoded(value) == Encoded(fields...));
        }
    }  // namespace

    TEST_CASE("Cell content encoding preserves geometry and identity field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const Sha256Digest value{};
            CheckFields(value, value.bytes);
        }
        {
            const Math::Vec2 v{};
            CheckFields(v, v.x, v.y);
        }
        {
            const Math::Vec3 v{};
            CheckFields(v, v.x, v.y, v.z);
        }
        {
            const Math::Quaternion v{};
            CheckFields(v, v.x, v.y, v.z, v.w);
        }
        {
            const Math::Transform v{};
            CheckFields(v, v.translation, v.rotation, v.scale);
        }
        {
            const WorldStreaming::StreamingCellId v{};
            CheckFields(v, v.x, v.y, v.z, v.lod, v.layer);
        }
        {
            const SceneCellPayloadIdentity v{};
            CheckFields(v, v.partition, v.cell, v.scene, v.revision);
        }
    }

    TEST_CASE("Cell content encoding preserves camera lighting and audio field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const CameraComponent v{};
            CheckFields(v, v.projection, v.verticalFieldOfViewRadians, v.orthographicHeight, v.nearPlane, v.farPlane, v.enabled);
        }
        {
            const LightComponent v{};
            CheckFields(v, v.kind, v.color, v.intensity, v.range, v.innerConeRadians, v.outerConeRadians, v.enabled);
        }
        {
            const Audio::AudioSoundDefinitionSchemaVersion v{};
            CheckFields(v, v.major, v.minor);
        }
        {
            const Audio::AudioSoundExtensionReference v{};
            CheckFields(v, v.contribution, v.definition, v.contractVersion);
        }
        {
            const Audio::AudioSoundReference v{};
            CheckFields(v, v.kind, v.target);
        }
        {
            const Audio::AudioConcurrencyPolicy v{};
            CheckFields(v, v.group, v.maxInstances, v.mode);
        }
        {
            const Audio::AudioSoundPlaybackDefaults v{};
            CheckFields(v, v.gain, v.pitch, v.bus, v.loop, v.spatialMode, v.enableDoppler, v.playOnStart, v.priority, v.concurrency);
        }
    }

    TEST_CASE("Cell content encoding preserves audio and UI field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const AudioSourceComponent v{};
            CheckFields(v, v.sound, v.playback, v.sceneLifecycle, v.enabled);
        }
        {
            const AudioListenerComponent v{};
            CheckFields(v, v.view, v.priority, v.weight, v.enabled);
        }
        {
            const Ui::UiCanvasAssetReference v{};
            CheckFields(v, v.asset, v.document, v.canvas, v.minimumRevision);
        }
        {
            const UiCanvasComponent v{};
            CheckFields(v, v.canvas);
        }
        {
            const NavigationLocalBounds v{};
            CheckFields(v, v.center, v.halfExtents);
        }
        {
            const NavigationSurfaceComponent v{};
            CheckFields(v, v.id, v.definition, v.schemaVersion, v.generation, v.bakeScope, v.localBounds, v.profiles, v.enabled);
        }
        {
            const NavigationRegionComponent v{};
            CheckFields(v, v.id, v.surface, v.schemaVersion, v.generation, v.localBounds, v.sourceSelection, v.mode, v.enabled);
        }
    }

    TEST_CASE("Cell content encoding preserves navigation geometry field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const NavigationCylinderVolume v{};
            CheckFields(v, v.center, v.radius, v.halfHeight);
        }
        {
            const NavigationModifierComponent v{};
            CheckFields(v, v.id, v.surface, v.schemaVersion, v.generation, v.volume, v.operation, v.area, v.traversalCost, v.enabled);
        }
        {
            const NavigationLinkEndpoint v{};
            CheckFields(v, v.surface, v.localPosition, v.connectionRadiusMeters);
        }
        {
            const NavigationLinkComponent v{};
            CheckFields(v, v.id, v.schemaVersion, v.generation, v.start, v.end, v.kind, v.direction, v.profiles, v.traversalCost,
                        v.enabled);
        }
        {
            const NavigationAgentComponent v{};
            CheckFields(v, v.schemaVersion, v.profile, v.filter, v.radiusOverride, v.movementCapability, v.enabled);
        }
        {
            const AI::AiCapabilitySet v{};
            CheckFields(v, v.bits);
        }
        {
            const AI::AiAgentComponent v{};
            CheckFields(v, v.agent, v.schemaVersion, v.startupPolicy, v.enabled);
        }
    }

    TEST_CASE("Cell content encoding preserves navigation agents and AI field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const AI::AiControllerComponent v{};
            CheckFields(v, v.controller, v.decisionAsset, v.blackboardSchema, v.decisionKind, v.requiredCapabilities, v.schemaVersion,
                        v.startupPolicy, v.enabled);
        }
        {
            const AuthoredPhysicsPose v{};
            CheckFields(v, v.translation, v.rotation);
        }
        {
            const PhysicsBodyReference v{};
            CheckFields(v, v.object, v.body);
        }
        {
            const AuthoredPhysicsNoMass v{};
            CheckFields(v);
        }
        {
            const AuthoredPhysicsMass v{};
            CheckFields(v, v.kilograms);
        }
        {
            const AuthoredPhysicsDensity v{};
            CheckFields(v, v.kilogramsPerCubicMeter);
        }
        {
            const RigidBodyComponent v{};
            CheckFields(v, v.id, v.body, v.schemaVersion, v.generation, v.motion, v.mass, v.initialLinearVelocity, v.initialAngularVelocity,
                        v.linearDampingPerSecond, v.angularDampingPerSecond, v.maximumLinearSpeed, v.maximumAngularSpeed, v.enabled);
        }
    }

    TEST_CASE("Cell content encoding preserves physics bodies and mass field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const PhysicsBoxCollider v{};
            CheckFields(v, v.halfExtentsMeters);
        }
        {
            const PhysicsSphereCollider v{};
            CheckFields(v, v.radiusMeters);
        }
        {
            const PhysicsCapsuleCollider v{};
            CheckFields(v, v.radiusMeters, v.cylindricalHalfHeightMeters);
        }
        {
            const PhysicsStaticPlaneCollider v{};
            CheckFields(v, v.normal, v.signedDistanceMeters);
        }
        {
            const PhysicsShapeAssetReference v{};
            CheckFields(v, v.asset, v.subresource);
        }
        {
            const PhysicsColliderMaterialBinding v{};
            CheckFields(v, v.slot, v.material);
        }
        {
            const ColliderComponent v{};
            CheckFields(v, v.id, v.collider, v.schemaVersion, v.generation, v.body, v.source, v.localPose, v.scale, v.collisionProfile,
                        v.materials, v.sensor, v.enabled);
        }
    }

    TEST_CASE("Cell content encoding preserves physics colliders and materials field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const PhysicsConstraintBodyEndpoint v{};
            CheckFields(v, v.body, v.localFrame);
        }
        {
            const PhysicsConstraintWorldEndpoint v{};
            CheckFields(v, v.frame);
        }
        {
            const PhysicsFixedConstraint v{};
            CheckFields(v);
        }
        {
            const PhysicsDistanceConstraint v{};
            CheckFields(v, v.minimumMeters, v.maximumMeters);
        }
        {
            const PhysicsConstraintComponent v{};
            CheckFields(v, v.id, v.constraint, v.schemaVersion, v.generation, v.first, v.second, v.parameters, v.enabled);
        }
        {
            const Gameplay::BehaviorField v{};
            CheckFields(v, v.name, v.value);
        }
        {
            const Gameplay::BehaviorComponent v{};
            CheckFields(v, v.instanceId, v.typeId, v.schemaVersion, v.enabled, v.fields);
        }
    }

    TEST_CASE("Cell content encoding preserves constraints and gameplay field order", "[scene][incremental_cell_cook][encoding]") {
        {
            const Gameplay::SerializedComponent v{};
            CheckFields(v, v.typeId, v.schemaVersion, v.encoding, v.payload);
        }
        {
            const BoxMeshParameters v{};
            CheckFields(v, v.size);
        }
        {
            const SphereMeshParameters v{};
            CheckFields(v, v.radius, v.slices, v.stacks);
        }
        {
            const CapsuleMeshParameters v{};
            CheckFields(v, v.radius, v.totalHeight, v.radialSegments, v.hemisphereRings);
        }
        {
            const CylinderMeshParameters v{};
            CheckFields(v, v.radius, v.height, v.radialSegments);
        }
        {
            const ConeMeshParameters v{};
            CheckFields(v, v.radius, v.height, v.radialSegments);
        }
        {
            const PlaneMeshParameters v{};
            CheckFields(v, v.size);
        }
    }

    TEST_CASE("Cell content encoding preserves mesh descriptors and scene schemas field order",
              "[scene][incremental_cell_cook][encoding]") {
        {
            const QuadMeshParameters v{};
            CheckFields(v, v.size);
        }
        {
            const PrimitiveMeshDescriptor v{};
            CheckFields(v, v.type, v.version, v.parameters);
        }
        {
            const RuntimeComponentSet v{};
            CheckFields(v, v.camera, v.light, v.audioSource, v.audioListener, v.uiCanvas, v.navigationSurface, v.navigationRegion,
                        v.navigationModifier, v.navigationLink, v.navigationAgent, v.aiAgent, v.aiController, v.rigidBody, v.colliders,
                        v.physicsConstraints, v.behaviors, v.gameplayComponents);
        }
        {
            const RuntimeEntityDefinition v{};
            CheckFields(v, v.object, v.parent, v.localTransform, v.primitiveMesh, v.components);
        }
        {
            const SceneAssetDependency v{};
            CheckFields(v, v.id, v.expectedType);
        }
        {
            const SceneCellComponentSchema v{};
            CheckFields(v, v.type, v.version);
        }
        {
            const SceneCellBehaviorSchema v{};
            CheckFields(v, v.type, v.version);
        }
    }

    TEST_CASE("Cell scalar encoding matches an independently packed little endian SHA256 fixture",
              "[scene][incremental_cell_cook][encoding]") {
        enum class Sample : std::uint8_t {
            Value = 3
        };
        CHECK(FormatSha256(Encoded(std::uint32_t{0x10203}, true, Sample::Value, 1.5F, -2.5)) ==
              "sha256:f27f5598a708e8ac0f916f1b60f23799d6077472b46eba096ec3086cd2975dcd");
        CHECK(Encoded(std::monostate{}) == ComputeSha256({}));
        CHECK(Encoded(std::optional<std::uint64_t>{}) == Encoded(false));
        CHECK(Encoded(std::optional<std::uint64_t>{7}) == Encoded(true, std::uint64_t{7}));
        CHECK(Encoded(std::variant<std::monostate, std::uint64_t>{std::uint64_t{7}}) == Encoded(std::size_t{1}, std::uint64_t{7}));
        CHECK(Encoded(std::vector<std::uint64_t>{2, 3}) == Encoded(std::size_t{2}, std::uint64_t{2}, std::uint64_t{3}));
    }

    TEST_CASE("Cell encoding preserves binary string length and chunks without truncation", "[scene][incremental_cell_cook][encoding]") {
        const std::string text{"a\0b", 3};
        const std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{}, std::byte{'b'}};
        CHECK(Encoded(text) == Encoded(bytes));
        CHECK(Encoded(text) != Encoded(std::string{"a"}));
        const std::string large(65537, 'x');
        std::vector<std::byte> stream(8 + large.size(), std::byte{'x'});
        for (unsigned i = 0; i < 8; ++i)
            stream[i] = static_cast<std::byte>(static_cast<std::uint64_t>(large.size()) >> (8U * i));
        CHECK(Encoded(large) == ComputeSha256(stream));
    }

    TEST_CASE("Cell key ceilings and cancellation never return a partial digest", "[scene][incremental_cell_cook][encoding]") {
        const CancellationToken cancellation;
        CellCookDetail::ContentHasher exact{cancellation, 8};
        exact.Add(std::uint64_t{7});
        REQUIRE(exact.Finish().HasValue());
        CellCookDetail::ContentHasher shortScalar{cancellation, 7};
        shortScalar.Add(std::uint64_t{7});
        REQUIRE(shortScalar.Finish().HasError());
        CHECK(shortScalar.Finish().ErrorValue().code.Value() == SceneCellPayloadErrors::CapacityExceeded.code.Value());
        CellCookDetail::ContentHasher shortSequence{cancellation, 16};
        shortSequence.Add(std::vector<std::uint64_t>{1, 2});
        REQUIRE(shortSequence.Finish().HasError());
        CancellationSource source;
        const auto token = source.Token();
        CellCookDetail::ContentHasher cancelled{token, 64};
        cancelled.Add(std::uint64_t{7});
        source.RequestCancellation();
        cancelled.Add(std::string{"unpublished"});
        REQUIRE(cancelled.Finish().HasError());
        CHECK(cancelled.Finish().ErrorValue().code.Value() == SceneCellPayloadErrors::Cancelled.code.Value());
    }
}  // namespace Horo::Runtime
