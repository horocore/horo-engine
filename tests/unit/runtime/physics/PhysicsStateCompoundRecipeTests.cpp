#include "Horo/Foundation/Sha256.h"
#include "PhysicsStateWorldTestSupport.h"

#include <algorithm>

namespace Horo::Physics::StateTest {
    namespace {
        ShapeHandle Wrap(PhysicsWorld &world, ShapeHandle child, const PhysicsPose &pose) {
            const std::array<PhysicsSceneShapeInstance, 1> children{{{child, pose}}};
            return Take(world.CreateSceneCompoundShape(children));
        }

        void ChangeBodies(WorldFixture &fixture, ShapeHandle shape) {
            const auto source = Take(PhysicsCommandSourceId::Create(9));
            const auto next = fixture.world->PublishedTick().completedTick + 1;
            for (std::size_t index = 1; index < fixture.bodies.size(); ++index) {
                const auto body = fixture.bodies[index].body;
                Take(fixture.world->QueueStructuralCommand({.order = {.simulationTick = next,
                                                                      .worldGeneration = body.world.Value(),
                                                                      .sceneGeneration = 7,
                                                                      .targetKind = PhysicsCommandTargetKind::Body,
                                                                      .targetIdentity = body.slot.index + 1ULL,
                                                                      .commandKind = PhysicsStructuralCommandKind::Change,
                                                                      .source = source,
                                                                      .sourceSequence = index},
                                                            .bodyMutation = PhysicsBodyMutation{.body = body, .shape = shape}}));
            }
            fixture.Advance();
            for (std::size_t index = 1; index < fixture.bodies.size(); ++index)
                REQUIRE(Take(fixture.world->ReadSceneBodyPolicy(fixture.bodies[index].body)).shape == shape);
        }
    }  // namespace

    TEST_CASE("Real identity single-child compound may reuse native geometry without a new allocation and still admits state",
              "[physics][native][state][geometry][alias]") {
        WorldFixture fixture;
        const auto parent = Wrap(*fixture.world, fixture.shapes[1].shape, {});
        const std::array<PhysicsStateShapeBinding, 3> shapes{{fixture.shapes[0], fixture.shapes[1], {400, parent}}};
        auto configuration = fixture.Configuration();
        configuration.shapes = shapes;
        fixture.content = std::make_shared<const std::string>(*fixture.content + ";identitySingleChild=box");
        configuration.contentLease = fixture.content;
        configuration.content = Horo::ComputeSha256(std::as_bytes(std::span{fixture.content->data(), fixture.content->size()}));
        Take(fixture.world->ConfigureStateSnapshots(configuration));
        const auto snapshot = fixture.Capture();
        auto candidate = Take(fixture.world->PrepareStateRestore(snapshot));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        const auto restored = fixture.Capture();
        CHECK(std::ranges::equal(restored->Bytes(), snapshot->Bytes()));
    }

    TEST_CASE("Real translated wrapper path survives intermediate Horo shape retirement and restores warmed solver state",
              "[physics][native][state][geometry][retirement]") {
        WorldFixture fixture;
        std::array<ShapeHandle, 4> retired;
        auto current = fixture.shapes[1].shape;
        for (std::size_t index = 0; index < retired.size(); ++index) {
            retired[index] = current;
            current = Wrap(*fixture.world, current, {.translation = {.01F, 0, 0}});
        }
        ChangeBodies(fixture, current);
        auto retirement = Take(fixture.world->PrepareSceneBodies(std::span<const PhysicsSceneBodyDescriptor>{}));
        Check(retirement->PrepareRetirement({}, retired, {}));
        Check(retirement->ValidatePublication());
        retirement->Publish();
        retirement.reset();
        fixture.Advance();  // A real complete tick removes the prior old-shape exit batch; no fabricated publication marker.
        fixture.content =
            std::make_shared<const std::string>(*fixture.content + ";wrapperChain=4*translate(.01,0,0);retiredIntermediates=4");
        const std::array<PhysicsStateShapeBinding, 2> shapes{{fixture.shapes[0], {400, current}}};
        auto configuration = fixture.Configuration();
        configuration.shapes = shapes;
        Take(fixture.world->ConfigureStateSnapshots(configuration));
        auto snapshot = fixture.Capture();
        REQUIRE_FALSE(snapshot->Solver().manifolds.empty());
        fixture.Advance();
        auto expected = fixture.Capture();
        auto candidate = Take(fixture.world->PrepareStateRestore(snapshot));
        Check(candidate.ValidatePublication());
        candidate.Publish();
        fixture.Advance();
        const auto actual = fixture.Capture();
        CHECK(std::ranges::equal(actual->Bytes(), expected->Bytes()));
    }
}  // namespace Horo::Physics::StateTest
