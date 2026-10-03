#include "CanonicalPhysicsRuntimeInternal.h"
#include "PhysicsTestUtils.h"

#include <Jolt/Physics/StateRecorderImpl.h>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Uses native replay storage to corrupt a copied velocity without invoking an asserting setter or a solver. */
        void CorruptVelocityForReadback(JPH::Body &body) {
            JPH::StateRecorderImpl state;
            body.GetMotionProperties()->SaveState(state);
            auto data = state.GetData();
            const auto bytes = std::bit_cast<std::array<char, sizeof(float)>>(std::numeric_limits<float>::quiet_NaN());
            REQUIRE(data.size() >= bytes.size());
            // The pinned native recorder writes linear velocity first. Preserve every remaining field.
            std::ranges::copy(bytes, data.begin());
            state.Clear();
            state.WriteBytes(data.data(), data.size());
            state.Rewind();
            body.GetMotionProperties()->RestoreState(state);
            REQUIRE_FALSE(state.IsFailed());
        }

        /** @brief Owns a real native world that is never stepped after deliberate test corruption. */
        struct NativeContainmentFixture final {
            NativeContainmentFixture() {
                runtime = CreateCanonicalRuntime().Value();
                world = CreateCanonicalWorld(runtime, Test::SmallWorldSettings()).Value();
            }

            NativeContainmentFixture(const NativeContainmentFixture &) = delete;
            NativeContainmentFixture &operator=(const NativeContainmentFixture &) = delete;

            ~NativeContainmentFixture() {
                DestroyCanonicalWorld(world);
                DestroyCanonicalRuntime(runtime);
            }

            CanonicalRuntimeHandle runtime;
            CanonicalWorldHandle world;
        };
    }  // namespace

    TEST_CASE("Native readback rejects corrupt velocity and quarantine retires joint filters", "[physics][native][nonfinite]") {
        NativeContainmentFixture fixture;
        const PhysicsWorldId owner = PhysicsWorldId::Create(938).Value();
        const auto shape = CreateCanonicalSceneShape(fixture.world, owner, PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor descriptor;
        descriptor.shape = shape;
        descriptor.motion = PhysicsMotionType::Dynamic;
        descriptor.mass = PhysicsMass{2};
        const auto corrupt = CreateCanonicalSceneBody(fixture.world, owner, {descriptor, false}).Value();
        descriptor.pose.translation.x = 4;
        const auto healthy = CreateCanonicalSceneBody(fixture.world, owner, {descriptor, false}).Value();
        PhysicsConstraintDescriptor joint;
        joint.first = {corrupt, {}};
        joint.second = PhysicsBodyAnchor{healthy, {}};
        joint.parameters = PhysicsFixedConstraint{};
        REQUIRE(CreateCanonicalSceneConstraint(fixture.world, owner, joint).HasValue());
        REQUIRE(CreateCanonicalSceneConstraint(fixture.world, owner, joint).HasValue());
        auto &native = *static_cast<CanonicalWorld *>(fixture.world.value);
        REQUIRE(native.scene.disabledJointCollisionPairs.size() == 1);
        {
            JPH::BodyLockWrite lock(native.native.system->GetBodyLockInterfaceNoLock(), native.scene.bodies.front().nativeBody);
            REQUIRE(lock.Succeeded());
            CorruptVelocityForReadback(lock.GetBody());
        }
        Test::RequireError(ReadCanonicalSceneBodyReconciliation(fixture.world, owner, corrupt), PhysicsErrors::BodyStateNonFinite);
        std::size_t cursor{};
        const auto finding = FindCanonicalNonFiniteBody(fixture.world, false, cursor);
        REQUIRE(finding.has_value());
        REQUIRE(finding->body == corrupt);
        QuarantineCanonicalSceneBody(fixture.world, corrupt, {});
        REQUIRE(native.scene.disabledJointCollisionPairs.empty());
        REQUIRE(native.scene.constraints.empty());
        REQUIRE(native.scene.bodies.size() == 1);
        REQUIRE(ReadCanonicalSceneBodyReconciliation(fixture.world, owner, healthy).HasValue());
        Test::RequireError(ReadCanonicalSceneBodyPolicy(fixture.world, owner, corrupt), PhysicsErrors::HandleStale);
    }
}  // namespace Horo::Physics::Detail
