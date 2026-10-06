#pragma once

#include "CanonicalPhysicsRuntime.h"
#include "CanonicalPhysicsRuntimeDiagnostics.h"
#include "CanonicalPhysicsRuntimeQuery.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Physics::Detail {
    /**
     * @brief Applies the admitted mass policy to native body settings before allocation.
     * @param settings Native creation settings to update.
     * @param mass Validated Horo mass policy.
     * @return Success, or DescriptorInvalid when density cannot derive a finite mass.
     */
    [[nodiscard]] Result<void> ApplyCanonicalMassPolicy(JPH::BodyCreationSettings &settings, const PhysicsMassPolicy &mass);

    /** @brief Process-owned Jolt registration and resource accounting. */
    struct CanonicalRuntime final {
        CanonicalRuntime() = default;
        CanonicalRuntime(const CanonicalRuntime &) = delete;
        CanonicalRuntime &operator=(const CanonicalRuntime &) = delete;
        ~CanonicalRuntime();

        bool typesRegistered{};
        JPH::TraceFunction priorTrace{};
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailedFunction priorAssertFailed{};
#endif
        CanonicalResourceCounts resources;
        std::unique_ptr<JPH::Factory> factory;
    };

    /** @brief Load-time compiled immutable layer rows; row zero always rejects simulation. */
    struct CanonicalSimulationTable final {
        static constexpr std::size_t RowCount = 65;
        PhysicsWorldSimulationBinding binding;
        std::array<CollisionLayerDefinition, RowCount - 1> layers;
        std::size_t layerCount{};
        std::array<SimulationPairResponse, RowCount * RowCount> responses{};

        /** @brief Borrows the exact normalized profile during the joined world operation; never substitutes the default. */
        [[nodiscard]] const CollisionProfileDefinition *Profile(const CollisionProfileId id) const noexcept {
            if (!binding.schema)
                return nullptr;
            const auto profiles = binding.schema->Profiles();
            const auto found = std::ranges::lower_bound(profiles, id, {}, &CollisionProfileDefinition::id);
            return found != profiles.end() && found->id == id ? std::to_address(found) : nullptr;
        }

        /** @brief Borrows an exact enabled query channel; missing or disabled identity remains unavailable. */
        [[nodiscard]] const CollisionQueryChannelDefinition *Channel(const PhysicsQueryChannelId id) const noexcept {
            if (!binding.schema)
                return nullptr;
            const auto channels = binding.schema->QueryChannels();
            const auto found = std::ranges::lower_bound(channels, id, {}, &CollisionQueryChannelDefinition::id);
            return found != channels.end() && found->id == id && found->enabled ? std::to_address(found) : nullptr;
        }

        /** @brief Returns copied query response, or absence for unknown profile/channel; disabled querying is closed. */
        [[nodiscard]] std::optional<CollisionQueryResponse> QueryResponse(const CollisionProfileId id,
                                                                          const PhysicsQueryChannelId channel) const noexcept {
            const auto *profile = Profile(id);
            if (!profile || !Channel(channel))
                return std::nullopt;
            if (!profile->queryEnabled)
                return CollisionQueryResponse::Ignore;
            const auto found = std::ranges::lower_bound(profile->queryResponses, channel, {}, &CollisionProfileQueryResponse::channel);
            return found != profile->queryResponses.end() && found->channel == channel
                       ? std::optional<CollisionQueryResponse>{found->response}
                       : std::nullopt;
        }

        /** @brief Translates only a live compiled row to project identity; row zero is not an authored layer. */
        [[nodiscard]] CollisionLayerId Layer(const JPH::ObjectLayer row) const noexcept {
            return row != 0 && row <= layerCount ? layers[row - 1].id : CollisionLayerId{};
        }

        /** @brief Authoritative captured filter generation, independent of query fixture inventory revisions. */
        [[nodiscard]] std::uint64_t SchemaGeneration() const noexcept {
            return binding.generation;
        }

        [[nodiscard]] SimulationPairResponse Response(const JPH::ObjectLayer first, const JPH::ObjectLayer second) const noexcept {
            if (first == 0 || second == 0 || first >= RowCount || second >= RowCount)
                return SimulationPairResponse::Ignore;
            return responses[static_cast<std::size_t>(first) * RowCount + second];
        }
    };

    class CanonicalBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
    public:
        [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
            return 1;
        }

        [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
            return JPH::BroadPhaseLayer{0};
        }
    };

    class CanonicalObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        explicit CanonicalObjectVsBroadPhase(const CanonicalSimulationTable &table) noexcept : table_(table) {}

        [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer layer, JPH::BroadPhaseLayer) const override {
            for (std::size_t other = 1; other < CanonicalSimulationTable::RowCount; ++other)
                if (table_.Response(layer, static_cast<JPH::ObjectLayer>(other)) != SimulationPairResponse::Ignore)
                    return true;
            return false;
        }

    private:
        const CanonicalSimulationTable &table_;
    };

    class CanonicalObjectPairs final : public JPH::ObjectLayerPairFilter {
    public:
        explicit CanonicalObjectPairs(const CanonicalSimulationTable &table) noexcept : table_(table) {}

        [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer first, const JPH::ObjectLayer second) const override {
            return table_.Response(first, second) != SimulationPairResponse::Ignore;
        }

    private:
        const CanonicalSimulationTable &table_;
    };

    struct CanonicalWorld;
    class ContactCaptureRoute;

    /** @brief Copies native contact callbacks into the owner-thread event projection seam. */
    class CanonicalContactListener final : public JPH::ContactListener {
    public:
        explicit CanonicalContactListener(CanonicalWorld &world) noexcept : world_(world) {}

        JPH::ValidateResult OnContactValidate(const JPH::Body &body1, const JPH::Body &body2, JPH::RVec3Arg baseOffset,
                                              const JPH::CollideShapeResult &collision) override;

        void OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                            JPH::ContactSettings &settings) override;
        void OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                JPH::ContactSettings &settings) override;

    private:
        void Emit(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                  const JPH::ContactSettings &settings) const noexcept;

        CanonicalWorld &world_;
    };

    struct CanonicalSceneShapeRecord final {
        ShapeHandle handle;
        JPH::Ref<JPH::Shape> shape;
    };

    struct CanonicalSceneBodyRecord final {
        BodyHandle handle;
        JPH::BodyID nativeBody;
        PhysicsPose pose;
        PhysicsBodyDescriptor policy;
        bool sensor{};
        bool motionStorageReserved{};
        std::uint64_t sceneEntity{};
        std::optional<PhysicsSceneCollisionBinding> collision;
        std::optional<float> injectedStateForTesting;
        std::uint8_t injectedComponentForTesting{};
        bool injectPostStepForTesting{true};
    };

    struct CanonicalConstraintBodies final {
        const CanonicalSceneBodyRecord *first{};
        const CanonicalSceneBodyRecord *second{};
    };

    struct CanonicalSceneConstraintRecord final {
        ConstraintHandle handle;
        JPH::Ref<JPH::Constraint> constraint;
        BodyHandle first;
        BodyHandle second;
        JPH::BodyID firstBody;
        JPH::BodyID secondBody;
        PhysicsJointCollisionPolicy collisionPolicy{};
    };

    /** @brief Constructs a detached native constraint against resident or staged body records; never registers it. */
    [[nodiscard]] Result<CanonicalSceneConstraintRecord> PrepareCanonicalConstraintRecord(CanonicalWorldHandle world, PhysicsWorldId owner,
                                                                                          const PhysicsConstraintDescriptor &descriptor,
                                                                                          std::span<const CanonicalSceneBodyRecord> staged);

    /** @brief Native solver objects retained in dependency order for one world. */
    struct CanonicalWorldNativeState final {
        std::unique_ptr<JPH::TempAllocatorImpl> scratch;
        std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
        std::unique_ptr<JPH::PhysicsSystem> system;
    };

    /** @brief Scene-owned shape, body and constraint storage plus its bounded capacities. */
    struct CanonicalWorldSceneState final {
        CanonicalWorldSceneState(const std::uint32_t maximumBodies, const std::uint32_t maximumShapes,
                                 const std::uint32_t maximumConstraints)
            : maximumShapes(maximumShapes), maximumBodies(maximumBodies), maximumConstraints(maximumConstraints),
              nativeBodyIndices(maximumBodies, std::numeric_limits<std::size_t>::max()) {}

        std::uint32_t maximumShapes{};
        std::uint32_t maximumBodies{};
        std::uint32_t maximumConstraints{};
        std::vector<CanonicalSceneShapeRecord> shapes;
        std::vector<CanonicalSceneBodyRecord> bodies;
        std::vector<std::size_t> nativeBodyIndices;
        std::vector<CanonicalSceneConstraintRecord> constraints;
        std::vector<std::uint64_t> disabledJointCollisionPairs;

        std::uint32_t nextConstraintSlot{};
    };

    /** @brief Query fixtures, stable native-index mapping and reusable bounded collectors for one world. */
    struct CanonicalWorldQueryState final {
        static constexpr std::size_t InvalidFixtureIndex = std::numeric_limits<std::size_t>::max();

        CanonicalWorldQueryState(const std::uint32_t maximumFixtures, const std::uint32_t maximumBodies)
            : maximumFixtures(maximumFixtures), nativeFixtureIndices(maximumBodies, InvalidFixtureIndex) {}

        std::uint32_t maximumFixtures{};
        std::vector<CanonicalQueryFixtureRecord> fixtures;
        std::vector<std::size_t> nativeFixtureIndices;
        std::uint32_t nextFixtureGeneration{1};
        std::uint64_t querySchemaGeneration{1};
        CanonicalQueryStorage storage;
    };

    /** @brief Per-world native ownership and bounded scene/query storage. */
    struct CanonicalWorld final {
        CanonicalWorld(CanonicalRuntime &runtime, std::uint32_t maximumBodies, std::uint32_t maximumFixtures, std::uint32_t maximumShapes,
                       std::uint32_t maximumConstraints);
        CanonicalWorld(const CanonicalWorld &) = delete;
        CanonicalWorld &operator=(const CanonicalWorld &) = delete;
        ~CanonicalWorld();

        CanonicalRuntime &owner;
        CanonicalSimulationTable simulation;
        CanonicalBroadPhaseLayers broadPhaseLayers;
        CanonicalObjectVsBroadPhase objectVsBroadPhase;
        CanonicalObjectPairs objectPairs;
        CanonicalWorldNativeState native;
        CanonicalContactListener contactListener;
        DiagnosticInbox diagnostics;
        std::atomic<ContactCaptureRoute *> contactRoute{};
        std::uint32_t nextResourceSlot{}; /**< Never reused across scene shapes, scene bodies, or query fixtures. */
        CanonicalWorldSceneState scene;
        PhysicsContinuousCollisionObservation continuousCollision;
        std::weak_ptr<CanonicalSceneBodyBatchState> pendingBodyBatch;
        CanonicalWorldQueryState query;
    };

    /** @brief Rebuilds bounded resident indices at a joined publication boundary, never in a query callback. */
    inline void RebuildCanonicalSceneBodyIndices(CanonicalWorld &world) noexcept {
        std::ranges::fill(world.scene.nativeBodyIndices, std::numeric_limits<std::size_t>::max());
        for (std::size_t index = 0; index < world.scene.bodies.size(); ++index)
            world.scene.nativeBodyIndices[world.scene.bodies[index].nativeBody.GetIndex()] = index;
    }

    /** @brief Resolves explicit or inherited quality only after proving actual shape/motion/sensor eligibility. */
    [[nodiscard]] Result<JPH::EMotionQuality> ResolveCanonicalMotionQuality(const PhysicsContinuousCollisionPolicy &policy,
                                                                            const PhysicsBodyDescriptor &body, const JPH::Shape &shape,
                                                                            bool sensor);

    /** @brief Limits native contact routing to one joined fixed step and clears borrowed state on exit. */
    class ContactCaptureRoute final {
    public:
        ContactCaptureRoute(CanonicalWorld &world, const std::uint64_t simulationTick, const CanonicalContactSink sink) noexcept
            : world_(world), simulationTick_(simulationTick), sink_(sink) {
            world_.contactRoute.store(this, std::memory_order::seq_cst);
        }

        ContactCaptureRoute(const ContactCaptureRoute &) = delete;
        ContactCaptureRoute &operator=(const ContactCaptureRoute &) = delete;

        ~ContactCaptureRoute() {
            world_.contactRoute.store(nullptr, std::memory_order::seq_cst);
        }

        [[nodiscard]] const CanonicalContactSink &Sink() const noexcept {
            return sink_;
        }

        [[nodiscard]] std::uint64_t SimulationTick() const noexcept {
            return simulationTick_;
        }

    private:
        CanonicalWorld &world_;
        std::uint64_t simulationTick_{};
        CanonicalContactSink sink_;
    };

    [[nodiscard]] inline JPH::Vec3 ToNative(const Math::Vec3 value) noexcept {
        return {value.x, value.y, value.z};
    }

    [[nodiscard]] inline JPH::Quat ToNative(const Math::Quaternion value) noexcept {
        return {value.x, value.y, value.z, value.w};
    }

    [[nodiscard]] inline Result<JPH::Ref<JPH::Shape>> CreateNativeShape(const PhysicsShapeDescriptor &descriptor) {
        JPH::ShapeSettings::ShapeResult created;
        std::visit([&created]<typename Shape>(const Shape &shape) {
            using ShapeType = std::decay_t<Shape>;
            if constexpr (std::is_same_v<ShapeType, PhysicsBoxShape>)
                created = JPH::BoxShapeSettings(ToNative(shape.halfExtentsMeters)).Create();
            else if constexpr (std::is_same_v<ShapeType, PhysicsSphereShape>)
                created = JPH::SphereShapeSettings(shape.radiusMeters).Create();
            else if constexpr (std::is_same_v<ShapeType, PhysicsCapsuleShape>)
                created = JPH::CapsuleShapeSettings(shape.cylindricalHalfHeightMeters, shape.radiusMeters).Create();
            else {
                static_assert(std::is_same_v<ShapeType, PhysicsStaticPlaneShape>);
                created = JPH::PlaneShapeSettings(JPH::Plane(ToNative(shape.normal), shape.signedDistanceMeters)).Create();
            }
        }, descriptor);
        if (created.HasError())
            return Result<JPH::Ref<JPH::Shape>>::Failure(
                MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the admitted analytic query shape."));
        return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
    }

    [[nodiscard]] inline JPH::RVec3 ToNativePoint(const Math::Vec3 value) noexcept {
        return {value.x, value.y, value.z};
    }

    [[nodiscard]] inline JPH::EMotionType ToNativeMotion(const PhysicsMotionType motion) noexcept {
        switch (motion) {
            case PhysicsMotionType::Static:
                return JPH::EMotionType::Static;
            case PhysicsMotionType::Kinematic:
                return JPH::EMotionType::Kinematic;
            case PhysicsMotionType::Dynamic:
                return JPH::EMotionType::Dynamic;
        }
        return JPH::EMotionType::Static;
    }

    [[nodiscard]] inline JPH::EAllowedDOFs ToNativeAllowedDOFs(const PhysicsAxisLock lockedAxes) noexcept {
        JPH::EAllowedDOFs allowed = JPH::EAllowedDOFs::All;
        const auto remove = [&allowed, lockedAxes](const PhysicsAxisLock lock, const JPH::EAllowedDOFs native) {
            if (HasPhysicsAxisLock(lockedAxes, lock))
                allowed &= ~native;
        };
        remove(PhysicsAxisLock::TranslationX, JPH::EAllowedDOFs::TranslationX);
        remove(PhysicsAxisLock::TranslationY, JPH::EAllowedDOFs::TranslationY);
        remove(PhysicsAxisLock::TranslationZ, JPH::EAllowedDOFs::TranslationZ);
        remove(PhysicsAxisLock::RotationX, JPH::EAllowedDOFs::RotationX);
        remove(PhysicsAxisLock::RotationY, JPH::EAllowedDOFs::RotationY);
        remove(PhysicsAxisLock::RotationZ, JPH::EAllowedDOFs::RotationZ);
        return allowed;
    }

    [[nodiscard]] inline PhysicsPose ComposePhysicsPose(const PhysicsPose &parent, const PhysicsPose &local) noexcept {
        return {.translation = parent.translation + parent.rotation.Rotate(local.translation),
                .rotation = parent.rotation * local.rotation};
    }
}  // namespace Horo::Physics::Detail
