#include "CharacterWorldTestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct StepProbe final {
            float height{0.3F};
            bool ceiling{};
            bool steepLanding{};
            bool malformed{};
            bool overlapLanding{};
            bool curved{};
            bool corner{};
            bool reverseHits{};
            bool missingLanding{};
            CharacterWorld *shutdownWorld{};
            std::uint32_t calls{};

            static Result<CharacterOverlapProbeResult> Overlap(void *context, const CharacterOverlapProbeRequest &request) noexcept {
                auto &probe = *static_cast<StepProbe *>(context);
                return Result<CharacterOverlapProbeResult>::Success(probe.overlapLanding && request.position.x > 0.2F
                                                                        ? CharacterOverlapProbeResult{1, {0, 0.1F, 0}}
                                                                        : CharacterOverlapProbeResult{});
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<StepProbe *>(context);
                ++probe.calls;
                if (probe.shutdownWorld != nullptr && request.direction.y > 0.0F)
                    probe.shutdownWorld->Shutdown();
                CharacterSweepProbeResult result;
                float distance = request.maximumDistanceMeters + 1;
                Math::Vec3 normal;
                Math::Vec3 point;
                if (request.direction.y < -0.5F) {
                    const float floor = request.position.x > 0.2F ? probe.height : 0.0F;
                    distance = std::max(0.0F, request.position.y - 1.0F - floor);
                    normal = probe.steepLanding && floor > 0 ? Math::Vec3{-0.8660254F, 0.5F, 0} : Math::Vec3{0, 1, 0};
                    point = {request.position.x, floor, 0};
                } else if (request.direction.y > 0.5F && probe.ceiling) {
                    distance = 0.1F;
                    normal = {0, -1, 0};
                    point = request.position + Math::Vec3{0, 1.1F, 0};
                } else if (request.direction.x > 0.5F && request.position.y < 1.0F + probe.height + 0.019F) {
                    distance = std::max(0.0F, (0.2F - request.position.x) / request.direction.x);
                    normal = probe.curved ? Math::Vec3{-0.6F, 0.8F, 0} : Math::Vec3{-1, 0, 0};
                    point = {0.2F, probe.curved ? probe.height : 0.3F, 0};
                }
                if (distance <= request.maximumDistanceMeters) {
                    result.hitCount = 1;
                    result.hits[0] = {.shape = {request.physicsWorld, {9, 3}},
                                      .point = point,
                                      .normal = probe.malformed && request.direction.y > 0 ? Math::Vec3{} : normal,
                                      .response = Physics::PhysicsQueryResponse::Block,
                                      .distanceMeters = distance};
                }
                if (probe.missingLanding && request.direction.y < -0.5F && request.position.x > 0.2F)
                    result.hitCount = 0;
                if (probe.corner && request.direction.z > 0.5F) {
                    const float cornerDistance = std::max(0.0F, (0.2F - request.position.z) / request.direction.z);
                    if (cornerDistance <= request.maximumDistanceMeters) {
                        result.hits[result.hitCount++] = {.shape = {request.physicsWorld, {10, 3}},
                                                          .point = {request.position.x, 0.3F, 0.2F},
                                                          .normal = {0, 0, -1},
                                                          .response = Physics::PhysicsQueryResponse::Block,
                                                          .distanceMeters = cornerDistance};
                    }
                    if (probe.reverseHits && result.hitCount == 2)
                        std::swap(result.hits[0], result.hits[1]);
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) {
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

        [[nodiscard]] SpawnedActiveWorld StepWorld(const float stepHeight = 0.3F, const std::uint32_t queries = 64) {
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.work.maximumQueriesPerTick = queries;
            auto world = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.capsule = {0.5F, 0.5F};
            descriptor.collisionRootPosition = {0, 1.02F, 0};
            descriptor.maximumStepHeightMeters = stepHeight;
            const auto controller = world->CreateController(descriptor).Value();
            REQUIRE(world->Activate().HasValue());
            OverlapProbe placement;
            REQUIRE(world->SpawnController(controller, placement.Context(world->Descriptor())).HasValue());
            return {std::move(world), controller};
        }

        [[nodiscard]] Result<void> MoveStep(SpawnedActiveWorld &spawned, StepProbe &probe, const std::uint64_t tick = 1,
                                            const Math::Vec3 velocity = {30, 0, 0}) {
            auto input = FixedTick(tick);
            input.query = probe.Context(spawned.world->Descriptor(), tick);
            auto request = Movement(spawned.controller, tick, tick);
            request.desiredVelocityMetersPerSecond = velocity;
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            return spawned.world->AdvanceFixedTick(input);
        }

        TEST_CASE("Character complete step path commits eligible exact and near height boundaries", "[physics][character][step]") {
            const float height = GENERATE(0.1F, 0.299F, 0.3F);
            auto spawned = StepWorld();
            StepProbe probe{height};
            REQUIRE(MoveStep(spawned, probe).HasValue());
            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            REQUIRE(snapshot.movement.finalPosition.x == Catch::Approx(0.5F));
            REQUIRE(snapshot.movement.finalPosition.y == Catch::Approx(1.02F + height));
            REQUIRE(snapshot.movement.grounded);
            REQUIRE((static_cast<std::uint16_t>(snapshot.movement.collisions) &
                     static_cast<std::uint16_t>(CharacterCollisionFlags::Step)) != 0);
            REQUIRE(MoveStep(spawned, probe, 2, {}).HasValue());
            REQUIRE(spawned.world->ControllerTransform(spawned.controller).Value().position == snapshot.transform.position);
        }

        TEST_CASE("Character rejects excessive curved steps ceilings unwalkable landings and blocked clearance",
                  "[physics][character][step][boundary]") {
            auto spawned = StepWorld();
            StepProbe probe;
            SECTION("over height") {
                probe.height = 0.301F;
            }
            SECTION("curvature cannot bypass height") {
                probe.height = 0.301F;
                probe.curved = true;
            }
            SECTION("ceiling") {
                probe.ceiling = true;
            }
            SECTION("unwalkable landing") {
                probe.steepLanding = true;
            }
            SECTION("overlap landing") {
                probe.overlapLanding = true;
            }
            SECTION("ledge has no landing") {
                probe.missingLanding = true;
            }
            SECTION("disabled") {
                spawned = StepWorld(0);
            }
            REQUIRE(MoveStep(spawned, probe).HasValue());
            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            REQUIRE(snapshot.movement.finalPosition.x < 0.2F);
            REQUIRE(snapshot.movement.finalPosition.y <= 1.02F + 1.0e-5F);
            REQUIRE((static_cast<std::uint16_t>(snapshot.movement.collisions) &
                     static_cast<std::uint16_t>(CharacterCollisionFlags::Step)) == 0);
        }

        TEST_CASE("Character step down preserves bounded contact and leaves excessive ledges unsnapped",
                  "[physics][character][step][ledge]") {
            const float drop = GENERATE(0.299F, 0.3F, 0.301F, 2.0F);
            auto spawned = StepWorld();
            StepProbe probe{-drop};
            REQUIRE(MoveStep(spawned, probe).HasValue());
            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            if (drop <= 0.3F) {
                REQUIRE(snapshot.movement.grounded);
                REQUIRE(snapshot.movement.finalPosition.y == Catch::Approx(1.02F - drop));
            } else {
                REQUIRE_FALSE(snapshot.movement.grounded);
                REQUIRE(snapshot.movement.finalPosition.y == Catch::Approx(1.02F));
            }
        }

        TEST_CASE("Character step corner remains stable across ticks and permuted coincident blockers",
                  "[physics][character][step][corner][determinism]") {
            const bool reverse = GENERATE(false, true);
            auto spawned = StepWorld();
            StepProbe probe;
            probe.corner = true;
            probe.reverseHits = reverse;
            Math::Vec3 committed;
            for (std::uint64_t tick = 1; tick <= 8; ++tick) {
                REQUIRE(MoveStep(spawned, probe, tick, {30, 0, 30}).HasValue());
                const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
                REQUIRE(snapshot.transform.position.x < 0.2F);
                REQUIRE(snapshot.transform.position.z < 0.2F);
                REQUIRE(snapshot.transform.position.y == Catch::Approx(1.02F));
                if (tick > 1)
                    REQUIRE(snapshot.transform.position == committed);
                committed = snapshot.transform.position;
            }
            REQUIRE(committed.x == Catch::Approx(0.2F - 0.02F / std::sqrt(2.0F)));
            REQUIRE(committed.z == Catch::Approx(committed.x));
        }

        TEST_CASE("Character failed step queries preserve publication and enforce shared capacity",
                  "[physics][character][step][validation]") {
            auto spawned = StepWorld();
            StepProbe probe;
            SECTION("malformed up cast") {
                probe.ceiling = true;
                probe.malformed = true;
            }
            SECTION("budget") {
                spawned = StepWorld(0.3F, 2);
            }
            SECTION("shutdown") {
                probe.shutdownWorld = spawned.world.get();
            }
            const auto before = spawned.world->ControllerTransform(spawned.controller).Value();
            REQUIRE(MoveStep(spawned, probe).HasError());
            REQUIRE(spawned.world->PublishedTick().completedTick == 0);
            if (probe.shutdownWorld == nullptr)
                REQUIRE(spawned.world->ControllerTransform(spawned.controller).Value().position == before.position);
            else
                REQUIRE(spawned.world->State() == CharacterWorldState::Destroyed);
        }
    }  // namespace
}  // namespace Horo::Character
