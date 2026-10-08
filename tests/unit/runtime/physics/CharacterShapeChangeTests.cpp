#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct ShapeProbe final {
            std::uint32_t calls{};
            Physics::PhysicsCapsuleShape observed;
            Math::Vec3 position;
            bool blocked{};
            bool fail{};
            bool malformed{};
            CharacterWorld *shutdownWorld{};

            static Result<CharacterOverlapProbeResult> Run(void *context, const CharacterOverlapProbeRequest &request) noexcept {
                auto &probe = *static_cast<ShapeProbe *>(context);
                ++probe.calls;
                probe.observed = request.capsule;
                probe.position = request.position;
                if (probe.shutdownWorld != nullptr)
                    probe.shutdownWorld->Shutdown();
                if (probe.fail)
                    return Result<CharacterOverlapProbeResult>::Failure(MakeError(Physics::PhysicsErrors::QueryCancelled));
                if (probe.malformed)
                    return Result<CharacterOverlapProbeResult>::Success({0, {1, 0, 0}});
                return Result<CharacterOverlapProbeResult>::Success(
                    {probe.blocked ? 1U : 0U, probe.blocked ? Math::Vec3{0, 1, 0} : Math::Vec3{}});
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,  world.identity, world.physicsWorld,           this, Run, world.collisionFilterGeneration,
                        world.originGeneration, tick,           world.physicsSnapshotRevision};
            }
        };

        [[nodiscard]] SpawnedActiveWorld StanceWorld(const std::uint32_t maximumQueries = 0, const Math::Vec3 up = {0, 1, 0},
                                                     const Math::Vec3 position = {}) {
            auto values = Settings().Values();
            if (maximumQueries != 0)
                values.work.maximumQueriesPerTick = maximumQueries;
            const auto settings = CharacterWorldSettings::Capture(values).Value();
            auto world = std::move(CharacterWorld::Prepare(WorldDescriptor(), settings)).Value();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.up = up;
            descriptor.collisionRootPosition = position;
            descriptor.crouchedCapsule = Physics::PhysicsCapsuleShape{0.5F, 0.25F};
            const auto controller = world->CreateController(descriptor).Value();
            REQUIRE(world->Activate().HasValue());
            OverlapProbe probe;
            REQUIRE(world->SpawnController(controller, probe.Context(world->Descriptor())).HasValue());
            return {std::move(world), controller};
        }

        TEST_CASE("Character commits stance geometry once at the fixed tick without changing identity or authored policy",
                  "[physics][character][shape]") {
            auto active = StanceWorld();
            auto request = Movement(active.controller, 1, 1);
            request.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(request).HasValue());
            REQUIRE(active.world->ControllerDescriptor(active.controller).Value().capsule.cylindricalHalfHeightMeters == 0.5F);
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            const auto before = Tests::AllocationProbe::Count();
            const auto advanced = active.world->AdvanceFixedTick(input);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(advanced.HasValue());
            REQUIRE(after == before);
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.controller == active.controller);
            REQUIRE(snapshot.stance == CharacterStance::Crouched);
            REQUIRE(snapshot.capsule.cylindricalHalfHeightMeters == 0.25F);
            REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Applied);
            REQUIRE(snapshot.transform.position == probe.position);
            REQUIRE(probe.position == Math::Vec3{0, -0.25F, 0});
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond == Math::Vec3{});
            REQUIRE(snapshot.transform.groundingRevalidationRequired);
            REQUIRE_FALSE(snapshot.transform.platformAttached);
            REQUIRE(probe.calls == 1);
            REQUIRE(probe.observed.cylindricalHalfHeightMeters == 0.25F);
            REQUIRE(active.world->ControllerDescriptor(active.controller).Value().maximumStepHeightMeters == 0.3F);
            REQUIRE(active.world->ControllerDescriptor(active.controller).Value().capsule.cylindricalHalfHeightMeters == 0.5F);

            request = Movement(active.controller, 2, 2);
            request.stance = CharacterStanceIntent::Stand;
            REQUIRE(active.world->QueueMovementCommand(request).HasValue());
            input = FixedTick(2);
            input.query = probe.Context(active.world->Descriptor(), 2);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().stance == CharacterStance::Standing);
            REQUIRE(probe.observed.cylindricalHalfHeightMeters == 0.5F);
        }

        TEST_CASE("Character repeated stance toggles preserve the bottom on an arbitrary up axis", "[physics][character][shape]") {
            const Math::Vec3 up{1, 0, 0};
            auto active = StanceWorld(0, up, {2, 3, 4});
            const auto foot = active.world->ControllerTransform(active.controller).Value().position - up;
            ShapeProbe probe;
            for (std::uint64_t tick = 1; tick <= 128; ++tick) {
                auto command = Movement(active.controller, tick, tick);
                command.stance = tick % 2 == 0 ? CharacterStanceIntent::Stand : CharacterStanceIntent::Crouch;
                REQUIRE(active.world->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(tick);
                input.query = probe.Context(active.world->Descriptor(), tick);
                REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
                const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
                REQUIRE(snapshot.transform.position - up * (snapshot.capsule.radiusMeters + snapshot.capsule.cylindricalHalfHeightMeters) ==
                        foot);
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond == Math::Vec3{});
            }
            REQUIRE(probe.calls == 128);
        }

        TEST_CASE("Character blocked stand retains crouched center and Keep does not retry clearance", "[physics][character][shape]") {
            auto active = StanceWorld();
            ShapeProbe probe;
            for (std::uint64_t tick = 1; tick <= 4; ++tick) {
                auto command = Movement(active.controller, tick, tick);
                command.stance = tick == 1   ? CharacterStanceIntent::Crouch
                                 : tick == 3 ? CharacterStanceIntent::Keep
                                             : CharacterStanceIntent::Stand;
                probe.blocked = tick == 2;
                REQUIRE(active.world->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(tick);
                input.query = probe.Context(active.world->Descriptor(), tick);
                REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
                const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
                REQUIRE(snapshot.transform.position.y == (tick == 4 ? 0.0F : -0.25F));
                if (tick == 2) {
                    REQUIRE(probe.position.y == 0.0F);
                    REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Blocked);
                    REQUIRE(snapshot.stance == CharacterStance::Crouched);
                }
            }
            REQUIRE(probe.calls == 3);
        }

        TEST_CASE("Character validates shifted resize bounds before invoking Physics", "[physics][character][shape]") {
            Math::Vec3 position{};
            Physics::PhysicsCapsuleShape target{0.5F, 0.25F};
            SECTION("local-origin boundary") {
                position.y = -Physics::MaximumPhysicsLocalHalfExtentMeters;
            }
            SECTION("fixed-tick displacement bound") {
                target.cylindricalHalfHeightMeters = Settings().Values().work.maximumDisplacementMetersPerTick + 1.0F;
            }
            auto active = StanceWorld(0, {0, 1, 0}, position);
            auto command = Movement(active.controller, 1, 1);
            command.shapeChange = CharacterShapeChangeRequest{target};
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            RequireError(active.world->AdvanceFixedTick(input), CharacterErrors::PlacementInvalid);
            REQUIRE(probe.calls == 0);
            REQUIRE(active.world->ControllerTransform(active.controller).Value().position == position);
            REQUIRE(active.world->PublishedTick().completedTick == 0);
        }

        TEST_CASE("Character explicit radius resize preserves bottom without reporting resize as velocity", "[physics][character][shape]") {
            auto active = StanceWorld();
            auto command = Movement(active.controller, 1, 1);
            command.shapeChange = CharacterShapeChangeRequest{{0.75F, 0.5F}};
            command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(probe.position.y == 0.25F);
            REQUIRE(snapshot.transform.position.y - snapshot.capsule.radiusMeters - snapshot.capsule.cylindricalHalfHeightMeters == -1.0F);
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond == Math::Vec3{1, 0, 0});
        }

        TEST_CASE("Character publishes blocked or invalid shape outcomes while retaining effective dimensions",
                  "[physics][character][shape]") {
            auto active = StanceWorld();
            auto request = Movement(active.controller, 1, 1);
            request.shapeChange = CharacterShapeChangeRequest{{0.75F, 0.8F}};
            ShapeProbe probe;
            CharacterShapeChangeStatus expected = CharacterShapeChangeStatus::Invalid;
            SECTION("blocked radius and height growth") {
                probe.blocked = true;
                expected = CharacterShapeChangeStatus::Blocked;
            }
            SECTION("non-finite radius") {
                request.shapeChange->capsule.radiusMeters = std::numeric_limits<float>::quiet_NaN();
            }
            SECTION("zero height") {
                request.shapeChange->capsule.cylindricalHalfHeightMeters = 0;
            }
            SECTION("negative radius") {
                request.shapeChange->capsule.radiusMeters = -1;
            }
            SECTION("out of local profile") {
                request.shapeChange->capsule.radiusMeters = std::numeric_limits<float>::max();
            }
            SECTION("conflicting named stance") {
                request.stance = CharacterStanceIntent::Crouch;
            }
            REQUIRE(active.world->QueueMovementCommand(request).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.movement.shapeChange->status == expected);
            REQUIRE(snapshot.capsule.radiusMeters == 0.5F);
            REQUIRE(snapshot.capsule.cylindricalHalfHeightMeters == 0.5F);
            REQUIRE(snapshot.stance == CharacterStance::Standing);
            REQUIRE(probe.calls == (expected == CharacterShapeChangeStatus::Blocked ? 1U : 0U));
        }

        TEST_CASE("Character blocked standing retains crouch and requires a new explicit intent to retry", "[physics][character][shape]") {
            auto active = StanceWorld();
            ShapeProbe probe;
            auto command = Movement(active.controller, 1, 1);
            command.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            command = Movement(active.controller, 2, 2);
            command.stance = CharacterStanceIntent::Stand;
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            probe.blocked = true;
            input = FixedTick(2);
            input.query = probe.Context(active.world->Descriptor(), 2);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.stance == CharacterStance::Crouched);
            REQUIRE(snapshot.capsule.cylindricalHalfHeightMeters == 0.25F);
            REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Blocked);
            REQUIRE(active.world->QueueMovementCommand(Movement(active.controller, 3, 3)).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(3)).HasValue());
            snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.stance == CharacterStance::Crouched);
            REQUIRE_FALSE(snapshot.movement.shapeChange.has_value());
            REQUIRE(probe.calls == 2);
        }

        TEST_CASE("Character missing crouch profile publishes Invalid without a query", "[physics][character][shape]") {
            auto active = SpawnedActiveWorldWithController();
            auto command = Movement(active.controller, 1, 1);
            command.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Invalid);
            REQUIRE(snapshot.stance == CharacterStance::Standing);
        }

        TEST_CASE("Character shape queries fail transactionally on stale malformed failed or shutdown evidence",
                  "[physics][character][shape]") {
            auto active = StanceWorld();
            auto request = Movement(active.controller, 1, 1);
            request.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(request).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            bool shutdown{};
            SECTION("wrong tick") {
                input.query.tick = 2;
            }
            SECTION("wrong snapshot") {
                ++input.query.physicsSnapshotRevision;
            }
            SECTION("missing adapter") {
                input.query.overlap = nullptr;
            }
            SECTION("Physics error") {
                probe.fail = true;
            }
            SECTION("malformed evidence") {
                probe.malformed = true;
            }
            SECTION("shutdown in callback") {
                shutdown = true;
                probe.shutdownWorld = active.world.get();
            }
            const auto previous = active.world->ControllerTransform(active.controller).Value();
            const auto result = active.world->AdvanceFixedTick(input);
            REQUIRE(result.HasError());
            REQUIRE(active.world->PublishedTick().completedTick == 0);
            if (!shutdown) {
                REQUIRE(active.world->ControllerTransform(active.controller).Value().publicationRevision == previous.publicationRevision);
                REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).HasError());
            } else {
                REQUIRE(active.world->State() == CharacterWorldState::Destroyed);
                REQUIRE(active.world->ActiveControllerCount() == 0);
            }
        }

        TEST_CASE("Character retains custom geometry across Keep ticks and teleport clearance", "[physics][character][shape]") {
            auto active = StanceWorld();
            auto request = Movement(active.controller, 1, 1);
            request.shapeChange = CharacterShapeChangeRequest{{0.6F, 0.3F}};
            REQUIRE(active.world->QueueMovementCommand(request).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().stance == CharacterStance::Custom);
            REQUIRE(active.world->QueueMovementCommand(Movement(active.controller, 2, 2)).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE_FALSE(snapshot.movement.shapeChange.has_value());
            REQUIRE(snapshot.capsule.radiusMeters == 0.6F);
            REQUIRE(snapshot.stance == CharacterStance::Custom);
            const CharacterTeleportRequest teleport{active.controller, 3, {0, 0, 1}};
            REQUIRE(active.world->TeleportController(teleport, probe.Context(active.world->Descriptor(), 3)).HasValue());
            REQUIRE(probe.observed.radiusMeters == 0.6F);
            REQUIRE(probe.observed.cylindricalHalfHeightMeters == 0.3F);
        }

        TEST_CASE("Character shape replacement and no-op stance avoid replay and redundant clearance", "[physics][character][shape]") {
            auto active = StanceWorld();
            auto crouch = Movement(active.controller, 1, 1);
            crouch.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(crouch).HasValue());
            auto stand = Movement(active.controller, 1, 2);
            stand.stance = CharacterStanceIntent::Stand;
            REQUIRE(active.world->QueueMovementCommand(stand).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1)).HasValue());
            const auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            REQUIRE(snapshot.movement.sequence == 2);
            REQUIRE(snapshot.stance == CharacterStance::Standing);
            REQUIRE(snapshot.movement.shapeChange->status == CharacterShapeChangeStatus::Applied);
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().tick == 1);
        }

        TEST_CASE("Character discards a cleared shape candidate when a later controller fails", "[physics][character][shape]") {
            auto active = ActiveWorldWithControllers(2);
            OverlapProbe spawnProbe;
            for (const auto handle : active.controllers)
                REQUIRE(active.world->SpawnController(handle, spawnProbe.Context(active.world->Descriptor())).HasValue());
            auto first = Movement(active.controllers[0], 1, 1);
            first.shapeChange = CharacterShapeChangeRequest{{0.6F, 0.3F}};
            auto second = Movement(active.controllers[1], 1, 1);
            second.desiredVelocityMetersPerSecond = Math::Vec3{std::numeric_limits<float>::max(), 0, 0};
            REQUIRE(active.world->QueueMovementCommand(first).HasValue());
            REQUIRE(active.world->QueueMovementCommand(second).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasError());
            REQUIRE(probe.calls == 1);
            REQUIRE(active.world->ControllerLocomotionSnapshot(first.controller).HasError());
            REQUIRE(active.world->PublishedTick().completedTick == 0);
            // The next tick must resolve against the original geometry after the failed attempt.
            first = Movement(first.controller, 2, 2);
            first.shapeChange = CharacterShapeChangeRequest{{0.5F, 0.5F}};
            REQUIRE(active.world->QueueMovementCommand(first).HasValue());
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE(active.world->ControllerLocomotionSnapshot(first.controller).Value().capsule.radiusMeters == 0.5F);
        }

        TEST_CASE("Character shares the shape and sweep query budget and resets it after failure", "[physics][character][shape]") {
            auto active = StanceWorld(1);
            auto command = Movement(active.controller, 1, 1);
            command.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            input.query.sweep = [](void *, const CharacterSweepProbeRequest &) noexcept {
                return Result<CharacterSweepProbeResult>::Success({});
            };
            RequireError(active.world->AdvanceFixedTick(input), CharacterErrors::CapacityExceeded);
            REQUIRE(probe.calls == 1);
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).HasError());
            command = Movement(active.controller, 2, 2);
            command.stance = CharacterStanceIntent::Crouch;
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            input = FixedTick(2);
            input.query = probe.Context(active.world->Descriptor(), 2);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().stance == CharacterStance::Crouched);
        }

        TEST_CASE("Character same-tick sweep and grounding use the cleared candidate capsule", "[physics][character][shape]") {
            auto active = StanceWorld();
            auto command = Movement(active.controller, 1, 1);
            command.shapeChange = CharacterShapeChangeRequest{{0.6F, 0.3F}};
            command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            input.query.sweep = [](void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &evidence = *static_cast<ShapeProbe *>(context);
                evidence.observed = request.capsule;
                ++evidence.calls;
                return Result<CharacterSweepProbeResult>::Success({});
            };
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            REQUIRE(probe.calls == 5);
            REQUIRE(probe.observed.radiusMeters == 0.6F);
            REQUIRE(probe.observed.cylindricalHalfHeightMeters == 0.3F);
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().stance == CharacterStance::Custom);
        }

        TEST_CASE("Character publication validation rejects malformed or inconsistent shape state", "[physics][character][shape]") {
            auto active = StanceWorld();
            auto command = Movement(active.controller, 1, 1);
            command.shapeChange = CharacterShapeChangeRequest{{0.6F, 0.3F}};
            REQUIRE(active.world->QueueMovementCommand(command).HasValue());
            ShapeProbe probe;
            auto input = FixedTick(1);
            input.query = probe.Context(active.world->Descriptor(), 1);
            REQUIRE(active.world->AdvanceFixedTick(input).HasValue());
            auto snapshot = active.world->ControllerLocomotionSnapshot(active.controller).Value();
            const auto descriptor = active.world->ControllerDescriptor(active.controller).Value();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
            SECTION("zero effective capsule radius") {
                snapshot.capsule.radiusMeters = 0;
            }
            SECTION("unknown committed stance") {
                snapshot.stance = static_cast<CharacterStance>(255);
            }
            SECTION("shape outcome disagrees with snapshot geometry") {
                snapshot.capsule.radiusMeters = 0.7F;
            }
            SECTION("shape outcome disagrees with snapshot stance") {
                snapshot.stance = CharacterStance::Standing;
            }
            SECTION("unknown shape outcome") {
                snapshot.movement.shapeChange->status = static_cast<CharacterShapeChangeStatus>(255);
                RequireError(ValidateCharacterMovementResult(snapshot.movement, descriptor), CharacterErrors::DescriptorInvalid);
            }
            SECTION("malformed shape outcome capsule") {
                snapshot.movement.shapeChange->effectiveCapsule.radiusMeters = 0;
                RequireError(ValidateCharacterMovementResult(snapshot.movement, descriptor), CharacterErrors::DescriptorInvalid);
            }
            SECTION("unknown shape outcome stance") {
                snapshot.movement.shapeChange->effectiveStance = static_cast<CharacterStance>(255);
                RequireError(ValidateCharacterMovementResult(snapshot.movement, descriptor), CharacterErrors::DescriptorInvalid);
            }
            RequireError(ValidateCharacterLocomotionSnapshot(snapshot, descriptor), CharacterErrors::DescriptorInvalid);
            // The malformed detached copies cannot alter the authoritative publication.
            REQUIRE(active.world->ControllerLocomotionSnapshot(active.controller).Value().capsule.radiusMeters == 0.6F);
        }

        TEST_CASE("Character validates crouch profiles and rejects unspawned or stale shape producers", "[physics][character][shape]") {
            auto world = PreparedWorld();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.crouchedCapsule = Physics::PhysicsCapsuleShape{0.4F, 0.25F};
            REQUIRE(world->CreateController(descriptor).HasError());
            descriptor.crouchedCapsule = Physics::PhysicsCapsuleShape{0.5F, 0.5F};
            REQUIRE(world->CreateController(descriptor).HasError());
            descriptor.crouchedCapsule.reset();
            const auto handle = world->CreateController(descriptor).Value();
            REQUIRE(world->DestroyController(handle).HasValue());
            const auto replacement = world->CreateController(descriptor).Value();
            REQUIRE(world->Activate().HasValue());
            auto request = Movement(handle, 1, 1);
            request.stance = CharacterStanceIntent::Crouch;
            RequireError(world->QueueMovementCommand(request), CharacterErrors::HandleStale);
            request.controller = replacement;
            RequireError(world->QueueMovementCommand(request), CharacterErrors::InvalidState);
        }
    }  // namespace
}  // namespace Horo::Character
