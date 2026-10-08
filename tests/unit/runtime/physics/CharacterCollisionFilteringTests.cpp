#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        Physics::CollisionLayerId Layer(const std::uint8_t value = 1) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Physics::CollisionLayerId::FromBytes(bytes);
        }

        CharacterPhysicsQueryExpectations Expectations(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
            return {world.sceneGeneration,  world.identity, world.physicsWorld,           world.collisionFilterGeneration,
                    world.originGeneration, tick,           world.physicsSnapshotRevision};
        }

        struct FilterProbe final {
            std::array<CharacterCollisionSelectors, 8> seen{};
            std::size_t calls{};
            bool fail{};

            static Result<CharacterOverlapProbeResult> Overlap(void *context, const CharacterOverlapProbeRequest &request) noexcept {
                auto &probe = *static_cast<FilterProbe *>(context);
                probe.seen[probe.calls++] = request.selectors;
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<FilterProbe *>(context);
                probe.seen[probe.calls++] = request.selectors;
                if (probe.fail)
                    return Result<CharacterSweepProbeResult>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
                return Result<CharacterSweepProbeResult>::Success({});
            }

            CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        Overlap,
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Sweep};
            }
        };

        TEST_CASE("Character rejects malformed and foreign selectors before command admission", "[physics][character][filter]") {
            auto active = SpawnedActiveWorldWithController();
            auto command = Movement(active.controller, 1, 1);
            command.filterChange = CharacterCollisionSelectors{.requiredLayer = Physics::CollisionLayerId{}};
            RequireError(active.world->QueueMovementCommand(command), CharacterErrors::DescriptorInvalid);
            command.filterChange = CharacterCollisionSelectors{.requiredProfile = Physics::CollisionProfileId{}};
            RequireError(active.world->QueueMovementCommand(command), CharacterErrors::DescriptorInvalid);
            command.filterChange = CharacterCollisionSelectors{.excludedBody = Physics::BodyHandle{PhysicsWorldId(999), {1, 1}}};
            RequireError(active.world->QueueMovementCommand(command), Physics::PhysicsErrors::HandleWorldMismatch);
            command.filterChange = CharacterCollisionSelectors{.excludedBody = Physics::BodyHandle{}};
            REQUIRE(active.world->QueueMovementCommand(command).HasError());
            REQUIRE(active.world->TickStatistics().pendingCommands == 0);
        }

        TEST_CASE("Character rejects malformed initial filters without reserving a controller", "[physics][character][filter]") {
            auto world = PreparedWorld();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.selectors.requiredLayer = Physics::CollisionLayerId{};
            RequireError(world->CreateController(descriptor), CharacterErrors::DescriptorInvalid);
            REQUIRE(world->ActiveControllerCount() == 0);
        }

        TEST_CASE("Character filters apply to shape movement and snap only at their selected tick", "[physics][character][filter]") {
            auto active = SpawnedActiveWorldWithController();
            FilterProbe probe;
            auto change = Movement(active.controller, 2, 2);
            change.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            change.shapeChange = CharacterShapeChangeRequest{{0.25F, 0.5F}};
            change.filterChange = CharacterCollisionSelectors{.requiredLayer = Layer()};
            REQUIRE(active.world->QueueMovementCommand(change).HasValue());
            REQUIRE_FALSE(active.world->ControllerDescriptor(active.controller).Value().selectors.requiredLayer);
            REQUIRE(active.world->QueueMovementCommand(Movement(active.controller, 1, 1)).HasValue());
            auto first = FixedTick(1);
            first.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(first).HasValue());
            REQUIRE(probe.calls == 3);
            REQUIRE_FALSE(probe.seen[0].requiredLayer);
            probe.calls = 0;
            auto second = FixedTick(2);
            second.query = probe.Context(active.world->Descriptor(), 2);
            REQUIRE(active.world->AdvanceFixedTick(second).HasValue());
            REQUIRE(probe.calls == 5);
            for (std::size_t index{}; index < probe.calls; ++index)
                REQUIRE(probe.seen[index].requiredLayer == Layer());
            REQUIRE(active.world->ControllerDescriptor(active.controller).Value().selectors.requiredLayer == Layer());
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().selectors.requiredLayer == Layer());
            probe.calls = 0;
            REQUIRE(active.world->QueueMovementCommand(Movement(active.controller, 3, 3)).HasValue());
            auto third = FixedTick(3);
            third.query = probe.Context(active.world->Descriptor(), 3);
            REQUIRE(active.world->AdvanceFixedTick(third).HasValue());
            REQUIRE(probe.seen[0].requiredLayer == Layer());
        }

        TEST_CASE("Failed filter tick preserves committed filter and lifecycle closes admission", "[physics][character][filter]") {
            auto active = SpawnedActiveWorldWithController();
            FilterProbe probe;
            probe.fail = true;
            auto command = Movement(active.controller, 1, 1);
            command.filterChange = CharacterCollisionSelectors{.requiredLayer = Layer()};
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            RequireError(active.world->AdvanceFixedTick(input), Physics::PhysicsErrors::QuerySnapshotStale);
            REQUIRE_FALSE(active.world->ControllerDescriptor(active.controller).Value().selectors.requiredLayer);
            REQUIRE(active.world->PublishedTick().completedTick == 0);
            active.world->Shutdown();
            RequireError(active.world->QueueMovementCommand(Movement(active.controller, 2, 2)), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character native adapter fails explicitly for Null Physics without publishing a spawn",
                  "[physics][character][filter][lifecycle]") {
            auto runtime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Null).Value();
            auto physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
            REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
            auto character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, Settings()).Value();
            const auto handle = character->CreateController(ControllerDescriptor(character->Descriptor())).Value();
            REQUIRE(character->Activate().HasValue());
            CharacterPhysicsQueryAdapter adapter{*physics};
            RequireError(character->SpawnController(handle, adapter.Context(Expectations(character->Descriptor(), 0))),
                         Physics::PhysicsErrors::CapabilityUnavailable);
            REQUIRE(character->ControllerTransform(handle).HasError());
            character->Shutdown();
            physics->Shutdown();
        }

#if HORO_TEST_PHYSICS_NATIVE
        struct NativeCharacter final {
            std::unique_ptr<Physics::PhysicsRuntime> runtime{
                Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value()};
            std::unique_ptr<Physics::PhysicsWorld> physics{runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value()};
            std::unique_ptr<CharacterWorld> character;
            CharacterControllerHandle controller;

            NativeCharacter() {
                REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
                REQUIRE(physics
                            ->AdvanceFixedTick(
                                {.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
                character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, Settings()).Value();
            }

            Physics::PhysicsQueryFixture Add(
                const Math::Vec3 position, const bool trigger = false,
                const Physics::PhysicsQueryFixtureResponse response = Physics::PhysicsQueryFixtureResponse::Block,
                const std::uint8_t layer = 1) {
                const auto descriptor = ControllerDescriptor(character->Descriptor());
                const auto added = physics->CreateQueryFixture({.shape = Physics::PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                                                .pose = {.translation = position},
                                                                .layer = Layer(layer),
                                                                .profile = descriptor.collisionProfile,
                                                                .channel = descriptor.queryChannel,
                                                                .response = response,
                                                                .trigger = trigger});
                REQUIRE(added.HasValue());
                return added.Value();
            }

            void Spawn(const CharacterCollisionSelectors selectors = {}, const Math::Vec3 position = {}) {
                auto descriptor = ControllerDescriptor(character->Descriptor());
                descriptor.capsule = {0.25F, 0.5F};
                descriptor.collisionRootPosition = position;
                descriptor.selectors = selectors;
                controller = character->CreateController(descriptor).Value();
                REQUIRE(character->Activate().HasValue());
                CharacterPhysicsQueryAdapter adapter{*physics};
                REQUIRE(character->SpawnController(controller, adapter.Context(Expectations(character->Descriptor(), 0))).HasValue());
            }

            CharacterMovementResult Move(const Math::Vec3 velocity = {},
                                         const std::optional<CharacterCollisionSelectors> filter = std::nullopt) {
                const auto tick = character->PublishedTick().completedTick + 1;
                auto command = Movement(controller, tick, tick);
                command.desiredVelocityMetersPerSecond = velocity;
                command.filterChange = filter;
                REQUIRE(character->QueueMovementCommand(command).HasValue());
                CharacterPhysicsQueryAdapter adapter{*physics};
                auto input = FixedTick(tick);
                input.fixedDelta = Duration::FromNanoseconds(250'000'000);
                input.query = adapter.Context(Expectations(character->Descriptor(), tick));
                const auto advanced = character->AdvanceFixedTick(input);
                if (advanced.HasError())
                    UNSCOPED_INFO(advanced.ErrorValue().message);
                REQUIRE(advanced.HasValue());
                return character->ControllerLocomotionSnapshot(controller).Value().movement;
            }
        };

        TEST_CASE("Canonical Character triggers and overlaps never recover block or support", "[physics][character][filter][native]") {
            for (const int mode : {0, 1, 2}) {
                NativeCharacter active;
                const bool trigger = mode == 0;
                const auto response = mode == 0   ? Physics::PhysicsQueryFixtureResponse::Block
                                      : mode == 1 ? Physics::PhysicsQueryFixtureResponse::Overlap
                                                  : Physics::PhysicsQueryFixtureResponse::Ignore;
                active.Add({}, trigger, response);
                active.Add({1, 0, 0}, trigger, response);
                active.Add({0, -1.25F, 0}, trigger, response);
                active.Spawn();
                REQUIRE(active.character->ControllerTransform(active.controller).Value().position == Math::Vec3{});
                const auto moved = active.Move({4, 0, 0});
                REQUIRE(moved.finalPosition.x == Catch::Approx(1));
                REQUIRE(moved.collisions == CharacterCollisionFlags::None);
                REQUIRE_FALSE(moved.grounded);
                REQUIRE(moved.contactCount == 0);
            }
        }

        TEST_CASE("Canonical Character uses the same layer and body selectors for recovery movement and snap",
                  "[physics][character][filter][native]") {
            for (const int selector : {0, 1, 2}) {
                NativeCharacter active;
                const auto fixture = active.Add({});
                CharacterCollisionSelectors selectors;
                if (selector == 0)
                    selectors.requiredLayer = Layer(2);
                else if (selector == 1)
                    selectors.excludedBody = fixture.body;
                else
                    selectors.requiredProfile = Physics::CollisionProfileId::Parse("52345678-1234-4234-8234-123456789abc").Value();
                active.Spawn(selectors);
                REQUIRE(active.character->ControllerTransform(active.controller).Value().position == Math::Vec3{});
                const auto moved = active.Move({1, 0, 0});
                REQUIRE(moved.finalPosition.x == Catch::Approx(0.25F));
                REQUIRE_FALSE(moved.grounded);
                REQUIRE(moved.contactCount == 0);
            }
        }

        TEST_CASE("Canonical Character snap excludes trigger before choosing deeper solid ground", "[physics][character][filter][native]") {
            NativeCharacter active;
            active.Add({0, -1.26F, 0}, true);
            const auto solid = active.Add({0, -1.35F, 0});
            active.Spawn();
            const auto moved = active.Move();
            REQUIRE(moved.grounded);
            REQUIRE(moved.groundBody == solid.body);
            REQUIRE(moved.finalPosition.y == Catch::Approx(-0.08F).margin(0.005F));
            const auto ignored = active.Move({}, CharacterCollisionSelectors{.excludedBody = solid.body});
            REQUIRE_FALSE(ignored.grounded);
            REQUIRE_FALSE(ignored.groundBody);
        }

        TEST_CASE("Canonical Character capsule query capacity is exact and hot queries allocate nothing",
                  "[physics][character][filter][native][capacity]") {
            for (const std::uint32_t count : {MaximumCharacterSweepHits, MaximumCharacterSweepHits + 1}) {
                NativeCharacter active;
                const auto policy = ControllerDescriptor(active.character->Descriptor());
                Physics::PhysicsCompoundShapeDescriptor compound;
                for (std::uint32_t index{}; index < count; ++index)
                    compound.children.push_back({.geometry = Physics::PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                                 .subshape = Physics::PhysicsShapeSubresourceId::FromValue(index + 1),
                                                 .layer = Layer(),
                                                 .profile = policy.collisionProfile,
                                                 .channel = policy.queryChannel});
                REQUIRE(active.physics
                            ->CreateQueryFixture({.shape = std::move(compound),
                                                  .pose = {.translation = {1.5F, 0, 0}},
                                                  .layer = Layer(),
                                                  .profile = policy.collisionProfile,
                                                  .channel = policy.queryChannel})
                            .HasValue());
                active.Spawn();
                CharacterPhysicsQueryAdapter adapter{*active.physics};
                auto context = adapter.Context(Expectations(active.character->Descriptor(), 1));
                CharacterSweepProbeRequest request{active.controller,
                                                   61,
                                                   active.character->Descriptor().identity,
                                                   active.physics->Identity(),
                                                   {0.25F, 0.5F},
                                                   {},
                                                   {0, 1, 0},
                                                   {1, 0, 0},
                                                   2,
                                                   policy.collisionProfile,
                                                   policy.queryChannel};
                auto warm = context.sweep(context.context, request);
                if (count > MaximumCharacterSweepHits) {
                    RequireError(warm, Physics::PhysicsErrors::CapacityExceeded);
                    REQUIRE(active.character->ControllerTransform(active.controller).Value().position == Math::Vec3{});
                } else {
                    REQUIRE(warm.HasValue());
                    REQUIRE(warm.Value().hitCount == MaximumCharacterSweepHits);
                    const auto before = Tests::AllocationProbe::Count();
                    auto measured = context.sweep(context.context, request);
                    const auto after = Tests::AllocationProbe::Count();
                    REQUIRE(measured.HasValue());
                    REQUIRE(after == before);
                }
            }
        }

        TEST_CASE("Character excluded body identity cannot alias a replacement collider",
                  "[physics][character][filter][native][lifecycle]") {
            NativeCharacter active;
            const auto old = active.Add({1.5F, 0, 0});
            active.Spawn({.excludedBody = old.body});
            REQUIRE(active.physics->DestroyQueryFixture(old).HasValue());
            const auto replacement = active.Add({1.5F, 0, 0});
            REQUIRE(replacement.body != old.body);
            const auto moved = active.Move({8, 0, 0});
            REQUIRE(moved.finalPosition.x < 0.76F);
            REQUIRE(moved.contactCount > 0);
            REQUIRE(moved.contacts[0].body == replacement.body);
            active.character->Shutdown();
            active.physics->Shutdown();
        }

        TEST_CASE("Committed Character filters persist into teleport and clear explicitly",
                  "[physics][character][filter][native][lifecycle]") {
            NativeCharacter active;
            const auto obstacle = active.Add({1.5F, 0, 0});
            active.Spawn();
            const auto bypass = active.Move({8, 0, 0}, CharacterCollisionSelectors{.excludedBody = obstacle.body});
            REQUIRE(bypass.finalPosition.x == Catch::Approx(2));
            CharacterPhysicsQueryAdapter adapter{*active.physics};
            const auto teleport = active.character->TeleportController({active.controller, 2, {1.5F, 0, 0}},
                                                                       adapter.Context(Expectations(active.character->Descriptor(), 2)));
            REQUIRE(teleport.HasValue());
            REQUIRE(active.character->AdvanceFixedTick(FixedTick(2)).HasValue());
            active.Move({}, CharacterCollisionSelectors{});
            REQUIRE_FALSE(active.character->ControllerDescriptor(active.controller).Value().selectors.excludedBody);
            const auto blocked = active.character->TeleportController({active.controller, 4, {1.5F, 0, 0}},
                                                                      adapter.Context(Expectations(active.character->Descriptor(), 4)));
            RequireError(blocked, CharacterErrors::PlacementInvalid);
        }

        TEST_CASE("Canonical Character solid overlap recovers and solid wall stops movement", "[physics][character][filter][native]") {
            NativeCharacter active;
            const auto descriptor = ControllerDescriptor(active.character->Descriptor());
            // Keep support beneath the wall: this case checks solid recovery and blocking,
            // independently of the airborne sweep budget after walking off a finite ledge.
            REQUIRE(active.physics
                        ->CreateQueryFixture({.shape = Physics::PhysicsBoxShape{{4.0F, 0.5F, 4.0F}},
                                              .pose = {.translation = {0, -0.5F, 0}},
                                              .layer = Layer(1),
                                              .profile = descriptor.collisionProfile,
                                              .channel = descriptor.queryChannel})
                        .HasValue());
            active.Add({1.5F, 0.8F, 0});
            active.Spawn({}, {0, 0.6F, 0});
            REQUIRE(active.character->ControllerTransform(active.controller).Value().position.y > 0.74F);
            const auto moved = active.Move({8, 0, 0});
            REQUIRE(moved.finalPosition.x < 0.76F);
            REQUIRE(moved.contactCount > 0);
        }
#endif
    }  // namespace
}  // namespace Horo::Character
