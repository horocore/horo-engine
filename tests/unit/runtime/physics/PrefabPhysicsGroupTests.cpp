#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Prepared Physics group bodies stay detached until aggregate publication", "[physics][native][prefab]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8630).Value()).HasValue());
        PhysicsBodyDescriptor body;
        body.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{1.0F};
        const std::array descriptors{PhysicsSceneBodyDescriptor{body}, PhysicsSceneBodyDescriptor{body}};
        auto prepared = world->PrepareSceneBodies(descriptors);
        REQUIRE(prepared.HasValue());
        auto group = std::move(prepared).Value();
        REQUIRE(group->Handles().size() == 2);
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 0);
        CHECK(world->ReadSceneBodyReconciliation(group->Handles()[0]).HasError());
        CHECK(world->CreateSceneBody({body}).HasError());
        CHECK(world->CreateSceneShape(PhysicsBoxShape{}).HasError());
        CHECK(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                  .HasError());
        REQUIRE(group->ValidatePublication().HasValue());
        group->Publish();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 2);
        CHECK(world->ReadSceneBodyReconciliation(group->Handles()[0]).HasValue());
        group.reset();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 2);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
    }

    TEST_CASE("Physics group rollback and world teardown abort every detached native body", "[physics][native][prefab]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8631).Value()).HasValue());
        PhysicsBodyDescriptor body;
        body.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        const std::array descriptors{PhysicsSceneBodyDescriptor{body}};
        auto prepared = world->PrepareSceneBodies(descriptors);
        REQUIRE(prepared.HasValue());
        auto group = std::move(prepared).Value();
        const auto rolledBack = group->Handles().front();
        group.reset();
        CHECK(world->ReadSceneActivation().Value().staticBodies == 0);
        CHECK(world->ReadSceneBodyReconciliation(rolledBack).HasError());
        prepared = world->PrepareSceneBodies(descriptors);
        REQUIRE(prepared.HasValue());
        group = std::move(prepared).Value();
        CHECK(group->Handles().front() != rolledBack);
        world.reset();
        CHECK(group->ValidatePublication().HasError());
        group.reset();
    }

    TEST_CASE("Physics prepared groups respect resident capacity and refund native rollback capacity", "[physics][native][prefab]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8632).Value()).HasValue());
        PhysicsBodyDescriptor body;
        body.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        const auto capacity = Test::SmallWorldSettings().Values().world.capacity.maximumBodies;
        std::vector<PhysicsSceneBodyDescriptor> full(capacity, PhysicsSceneBodyDescriptor{body});
        auto prepared = world->PrepareSceneBodies(full);
        REQUIRE(prepared.HasValue());
        auto group = std::move(prepared).Value();
        REQUIRE(group->Handles().size() == capacity);
        CHECK(world->ReadSceneActivation().Value().staticBodies == 0);
        group.reset();
        prepared = world->PrepareSceneBodies(full);
        REQUIRE(prepared.HasValue());
        group = std::move(prepared).Value();
        REQUIRE(group->ValidatePublication().HasValue());
        group->Publish();
        CHECK(world->ReadSceneActivation().Value().staticBodies == capacity);
        group.reset();
        const std::array excess{PhysicsSceneBodyDescriptor{body}};
        CHECK(world->PrepareSceneBodies(excess).HasError());
        CHECK(world->ReadSceneActivation().Value().staticBodies == capacity);
    }

    TEST_CASE("Physics owned shapes and constraints remain detached and refund every rollback", "[physics][native][prefab][resources]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8633).Value()).HasValue());
        const std::array shapes{PhysicsSceneGroupShape{PhysicsShapeDescriptor{PhysicsBoxShape{}}},
                                PhysicsSceneGroupShape{std::vector<PhysicsSceneGroupShape::Child>{{.shape = 0, .localPose = {}}}}};
        PhysicsSceneGroupBody body;
        body.shape = 1;
        body.descriptor.body.motion = PhysicsMotionType::Dynamic;
        body.descriptor.body.mass = PhysicsMass{1.0F};
        ConstraintHandle stale;
        ShapeHandle rolledBack;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto prepared = world->PrepareSceneGroup(shapes, std::array{body});
            REQUIRE(prepared.HasValue());
            auto group = std::move(prepared).Value();
            REQUIRE(group->Shapes().size() == 2);
            REQUIRE(group->Handles().size() == 1);
            PhysicsConstraintDescriptor constraint;
            constraint.first.body = group->Handles().front();
            constraint.parameters = PhysicsHingeConstraint{};
            REQUIRE(group->PrepareConstraints(std::array{constraint}).HasValue());
            REQUIRE(group->Constraints().size() == 1);
            stale = group->Constraints().front();
            rolledBack = group->Shapes().front();
            CHECK(world->ReadSceneJointState(stale).HasError());
            CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 0);
            REQUIRE(group->ValidatePublication().HasValue());
            group.reset();
            CHECK(world->ReadSceneJointState(stale).HasError());
            const std::array invalid{PhysicsSceneShapeInstance{rolledBack, {}}};
            CHECK(world->CreateSceneCompoundShape(invalid).HasError());
        }
        auto prepared = world->PrepareSceneGroup(shapes, std::array{body});
        REQUIRE(prepared.HasValue());
        auto group = std::move(prepared).Value();
        PhysicsConstraintDescriptor constraint;
        constraint.first.body = group->Handles().front();
        constraint.parameters = PhysicsHingeConstraint{};
        REQUIRE(group->PrepareConstraints(std::array{constraint}).HasValue());
        REQUIRE(group->Constraints().front() != stale);
        REQUIRE(group->Shapes().front() != rolledBack);
        REQUIRE(group->ValidatePublication().HasValue());
        group->Publish();
        REQUIRE(world->ReadSceneJointState(group->Constraints().front()).HasValue());
        REQUIRE(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        group.reset();
        REQUIRE(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
    }

    TEST_CASE("Physics owned resource failure poisons publication without changing resident constraints",
              "[physics][native][prefab][resources]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8634).Value()).HasValue());
        PhysicsBodyDescriptor resident;
        resident.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        resident.motion = PhysicsMotionType::Dynamic;
        resident.mass = PhysicsMass{1.0F};
        const auto residentBody = world->CreateSceneBody({resident}).Value();
        PhysicsConstraintDescriptor previous;
        previous.first.body = residentBody;
        previous.parameters = PhysicsHingeConstraint{};
        const auto previousConstraint = world->CreateSceneConstraint(previous).Value();
        const auto previousCoordinate = world->ReadSceneJointState(previousConstraint).Value().coordinate;
        const std::array shapes{PhysicsSceneGroupShape{PhysicsShapeDescriptor{PhysicsBoxShape{}}}};
        const std::array bodies{PhysicsSceneGroupBody{.descriptor = {resident}, .shape = 0}};
        auto prepared = world->PrepareSceneGroup(shapes, bodies);
        REQUIRE(prepared.HasValue());
        auto group = std::move(prepared).Value();
        PhysicsConstraintDescriptor invalid;
        CHECK(group->PrepareConstraints(std::array{invalid}).HasError());
        CHECK(group->ValidatePublication().HasError());
        group->Publish();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        CHECK(world->ReadSceneJointState(previousConstraint).Value().coordinate == previousCoordinate);
        group.reset();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        CHECK(world->ReadSceneJointState(previousConstraint).Value().coordinate == previousCoordinate);
    }

    TEST_CASE("Physics group retirement closure preserves residents on abort and removes them only at publication",
              "[physics][native][prefab][retirement]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8635).Value()).HasValue());
        const std::array shapes{PhysicsSceneGroupShape{PhysicsShapeDescriptor{PhysicsBoxShape{}}}};
        PhysicsSceneGroupBody body;
        body.shape = 0;
        body.descriptor.body.motion = PhysicsMotionType::Dynamic;
        body.descriptor.body.mass = PhysicsMass{1.0F};
        auto initial = world->PrepareSceneGroup(shapes, std::array{body});
        REQUIRE(initial.HasValue());
        const auto resident = initial.Value()->Handles().front();
        const auto shape = initial.Value()->Shapes().front();
        PhysicsConstraintDescriptor constraint;
        constraint.first.body = resident;
        REQUIRE(initial.Value()->PrepareConstraints(std::array{constraint}).HasValue());
        const auto joint = initial.Value()->Constraints().front();
        REQUIRE(initial.Value()->ValidatePublication().HasValue());
        initial.Value()->Publish();
        initial.Value().reset();
        const auto retirement = [&] {
            auto prepared = world->PrepareSceneGroup({}, {});
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value()->PrepareRetirement(std::array{resident}, std::array{shape}, {}).HasValue());
            REQUIRE(prepared.Value()->RetiredConstraints().size() == 1);
            REQUIRE(prepared.Value()->RetiredConstraints().front() == joint);
            REQUIRE(prepared.Value()->ValidatePublication().HasValue());
            return std::move(prepared).Value();
        };
        auto prepared = retirement();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
        CHECK(world->ReadSceneBodyReconciliation(resident).HasValue());
        CHECK(world->ReadSceneJointState(joint).HasValue());
        prepared.reset();
        CHECK(world->ReadSceneBodyReconciliation(resident).HasValue());
        CHECK(world->ReadSceneJointState(joint).HasValue());
        prepared = retirement();
        prepared->Publish();
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 0);
        CHECK(world->ReadSceneBodyReconciliation(resident).HasError());
        CHECK(world->ReadSceneJointState(joint).HasError());
        prepared.reset();
        // Both native body and shape capacities can be reused; identities must not be rewound.
        auto replacement = world->PrepareSceneGroup(shapes, std::array{body});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value()->Handles().front() != resident);
        CHECK(replacement.Value()->Shapes().front() != shape);
    }

    TEST_CASE("Physics constraint-only groups bind resident bodies and rollback without a fabricated new body",
              "[physics][native][prefab][retirement]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(8636).Value()).HasValue());
        PhysicsBodyDescriptor body;
        body.shape = world->CreateSceneShape(PhysicsBoxShape{}).Value();
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{1.0F};
        const auto resident = world->CreateSceneBody({body}).Value();
        PhysicsConstraintDescriptor descriptor;
        descriptor.first.body = resident;
        auto prepared = world->PrepareSceneGroup({}, {});
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value()->Handles().empty());
        REQUIRE(prepared.Value()->PrepareConstraints(std::array{descriptor}).HasValue());
        const auto aborted = prepared.Value()->Constraints().front();
        CHECK(world->ReadSceneJointState(aborted).HasError());
        prepared.Value().reset();
        CHECK(world->ReadSceneBodyReconciliation(resident).HasValue());
        prepared = world->PrepareSceneGroup({}, {});
        REQUIRE(prepared.HasValue());
        REQUIRE(prepared.Value()->PrepareConstraints(std::array{descriptor}).HasValue());
        const auto published = prepared.Value()->Constraints().front();
        CHECK(published != aborted);
        REQUIRE(prepared.Value()->ValidatePublication().HasValue());
        prepared.Value()->Publish();
        CHECK(world->ReadSceneJointState(published).HasValue());
        CHECK(world->ReadSceneActivation().Value().awakeMovingBodies == 1);
    }
#endif
}  // namespace Horo::Physics
