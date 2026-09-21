#pragma once

#include "CanonicalPhysicsRuntime.h"
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
    struct AllocatorFunctions final {
        JPH::AllocateFunction allocate{};
        JPH::ReallocateFunction reallocate{};
        JPH::AlignedAllocateFunction alignedAllocate{};
    };

    extern AllocatorFunctions installedAllocatorFunctions;

    /** @brief Fixed non-blocking mailbox written by native callbacks and drained at an owner-thread safe point. */
    struct DiagnosticInbox final {
        /** @brief Appends a bounded owner-thread summary for messages rejected by callback contention. */
        static void AppendDroppedSummary(std::string &message, const std::uint32_t droppedCount) {
            if (droppedCount == 0)
                return;
            std::array<char, 16> countText{};
            const auto [countEnd, error] = std::to_chars(countText.data(), countText.data() + countText.size(), droppedCount);
            if (error != std::errc{})
                return;
            std::string suffix{" ["};
            suffix.append(countText.data(), static_cast<std::size_t>(countEnd - countText.data()));
            suffix.append(" additional messages dropped]");
            if (message.size() + suffix.size() > MaximumPhysicsDiagnosticMessageBytes)
                message.resize(MaximumPhysicsDiagnosticMessageBytes - suffix.size());
            message.append(suffix);
        }

        /** @brief Maps one private normalized classification to a stable Horo error. */
        static Error MakeDiagnosticError(const CanonicalDiagnosticKind kind, std::string message) {
            using enum CanonicalDiagnosticKind;
            switch (kind) {
                case Validation:
                    return MakeError(PhysicsErrors::SolverValidationMessage, std::move(message));
                case Assertion:
                    return MakeError(PhysicsErrors::SolverAssertionFailed, std::move(message));
                case Fatal:
                    return MakeError(PhysicsErrors::SolverFatalCondition, std::move(message));
            }
            return MakeError(PhysicsErrors::SolverFatalCondition, "Unknown canonical solver diagnostic classification.");
        }

        void RetainEmergency(const CanonicalDiagnosticKind kind) noexcept {
            const auto desired = static_cast<std::uint8_t>(static_cast<std::uint8_t>(kind) + std::uint8_t{1});
            std::uint8_t current = emergencyKind.load();
            while (current < desired && !emergencyKind.compare_exchange_weak(current, desired)) {
                // A producer changed the priority; retry only while this condition is more severe.
            }
        }

        void Submit(const CanonicalDiagnosticKind kind, const std::string_view message) noexcept {
            if (lock.test_and_set()) {
                dropped.fetch_add(1);
                if (kind != CanonicalDiagnosticKind::Validation)
                    RetainEmergency(kind);
                return;
            }
            if (!occupied || kind > retainedKind) {
                retainedKind = kind;
                const std::string_view evidence =
                    message.empty() ? std::string_view{"Native solver emitted an empty diagnostic message."} : message;
                const std::size_t count = std::min(evidence.size(), text.size() - 1);
                std::copy_n(evidence.data(), count, text.data());
                text[count] = '\0';
                size = count;
                occupied = true;
            }
            lock.clear();
        }

        [[nodiscard]] std::optional<Error> Drain() {
            if (lock.test_and_set())
                return MakeError(PhysicsErrors::SolverFatalCondition,
                                 "A native solver callback did not quiesce before the owner-thread drain boundary.");
            const std::uint8_t emergency = emergencyKind.exchange(0);
            if (!occupied && emergency == 0) {
                lock.clear();
                return std::nullopt;
            }
            CanonicalDiagnosticKind kind = occupied ? retainedKind : static_cast<CanonicalDiagnosticKind>(emergency - 1);
            std::string message =
                occupied ? std::string{text.data(), size} : std::string{"Native solver fatal evidence was bounded by callback contention."};
            if (occupied && emergency > static_cast<std::uint8_t>(kind) + 1) {
                kind = static_cast<CanonicalDiagnosticKind>(emergency - 1);
                message = "Native solver fatal evidence was bounded by callback contention.";
            }
            AppendDroppedSummary(message, dropped.exchange(0));
            occupied = false;
            size = 0;
            lock.clear();
            return MakeDiagnosticError(kind, std::move(message));
        }

        std::atomic_flag lock = ATOMIC_FLAG_INIT;
        std::atomic<std::uint32_t> dropped{};
        std::atomic<std::uint8_t> emergencyKind{};
        std::array<char, MaximumPhysicsDiagnosticMessageBytes + 1> text{};
        std::size_t size{};
        CanonicalDiagnosticKind retainedKind{CanonicalDiagnosticKind::Validation};
        bool occupied{};
    };

    std::atomic<DiagnosticInbox *> &ActiveDiagnosticInbox() noexcept;

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

    /** @brief Temporary closed filters until the collision-profile contract installs a validated table. */
    class ClosedBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
    public:
        [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
            return 1;
        }

        [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override {
            return JPH::BroadPhaseLayer{0};
        }
    };

    class ClosedObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
            return false;
        }
    };

    class ClosedObjectPairs final : public JPH::ObjectLayerPairFilter {
    public:
        [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override {
            return false;
        }
    };

    struct CanonicalQueryFixtureRecord final {
        PhysicsQueryFixture fixture;
        PhysicsQueryFixtureDescriptor descriptor;
        JPH::BodyID nativeBody;
        JPH::Ref<JPH::Shape> shape;
    };

    struct CanonicalSceneShapeRecord final {
        ShapeHandle handle;
        JPH::Ref<JPH::Shape> shape;
    };

    struct CanonicalSceneBodyRecord final {
        BodyHandle handle;
        JPH::BodyID nativeBody;
        PhysicsPose pose;
    };

    struct CanonicalConstraintBodies final {
        const CanonicalSceneBodyRecord *first{};
        const CanonicalSceneBodyRecord *second{};
    };

    struct CanonicalSceneConstraintRecord final {
        ConstraintHandle handle;
        JPH::Ref<JPH::Constraint> constraint;
    };

    /** @brief Fixed-capacity native collector; callbacks cannot allocate or mutate world structure. */
    template <typename Collector> class FixedQueryCollector final : public Collector {
    public:
        using ResultType = typename Collector::ResultType;

        FixedQueryCollector(ResultType *storage, const PhysicsQueryCollection collection) : values(storage), collection_(collection) {}

        void AddHit(const ResultType &hit) override {
            if (collection_ == PhysicsQueryCollection::Any) {
                if (count == 0)
                    values[count++] = hit;
                this->ForceEarlyOut();
                return;
            }
            if (count < MaximumPhysicsQueryHits)
                values[count++] = hit;
            else
                overflow = true;
        }

        ResultType *values{};
        std::size_t count{};
        bool overflow{};
        PhysicsQueryCollection collection_;
    };

    /** @brief Per-world native ownership in dependency order. */
    struct CanonicalWorld final {
        static constexpr std::size_t InvalidFixtureIndex = std::numeric_limits<std::size_t>::max();

        CanonicalWorld(CanonicalRuntime &runtime, std::uint32_t maximumBodies, std::uint32_t maximumFixtures, std::uint32_t maximumShapes,
                       std::uint32_t maximumConstraints);
        CanonicalWorld(const CanonicalWorld &) = delete;
        CanonicalWorld &operator=(const CanonicalWorld &) = delete;
        ~CanonicalWorld();

        CanonicalRuntime &owner;
        ClosedBroadPhaseLayers broadPhaseLayers;
        ClosedObjectVsBroadPhase objectVsBroadPhase;
        ClosedObjectPairs objectPairs;
        std::unique_ptr<JPH::TempAllocatorImpl> scratch;
        std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
        std::unique_ptr<JPH::PhysicsSystem> system;
        DiagnosticInbox diagnostics;
        std::uint32_t maximumFixtures{};
        std::uint32_t maximumShapes{};
        std::uint32_t maximumBodies{};
        std::uint32_t maximumConstraints{};
        std::vector<CanonicalSceneShapeRecord> sceneShapes;
        std::vector<CanonicalSceneBodyRecord> sceneBodies;
        std::vector<CanonicalSceneConstraintRecord> sceneConstraints;
        std::vector<CanonicalQueryFixtureRecord> fixtures;
        std::vector<std::size_t> nativeFixtureIndices;
        std::uint32_t nextSceneShapeSlot{};
        std::uint32_t nextSceneBodySlot{};
        std::uint32_t nextSceneConstraintSlot{};
        std::uint32_t nextFixtureSlot{};
        std::uint32_t nextFixtureGeneration{1};
        std::uint64_t querySchemaGeneration{1};
        std::array<JPH::CastRayCollector::ResultType, MaximumPhysicsQueryHits> rayQueryResults{};
        std::array<JPH::CollidePointCollector::ResultType, MaximumPhysicsQueryHits> pointQueryResults{};
        std::array<JPH::CollideShapeCollector::ResultType, MaximumPhysicsQueryHits> overlapQueryResults{};
        std::array<JPH::CastShapeCollector::ResultType, MaximumPhysicsQueryHits> sweepQueryResults{};
        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> queryCandidates{};
    };

    /** @brief Reusable fixed-capacity collectors for one owner-thread query execution. */
    struct CanonicalQueryCollectors final {
        CanonicalQueryCollectors(CanonicalWorld &world, PhysicsQueryCollection collection)
            : ray(world.rayQueryResults.data(), collection), point(world.pointQueryResults.data(), collection),
              overlap(world.overlapQueryResults.data(), collection), sweep(world.sweepQueryResults.data(), collection) {}

        FixedQueryCollector<JPH::CastRayCollector> ray;
        FixedQueryCollector<JPH::CollidePointCollector> point;
        FixedQueryCollector<JPH::CollideShapeCollector> overlap;
        FixedQueryCollector<JPH::CastShapeCollector> sweep;
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
