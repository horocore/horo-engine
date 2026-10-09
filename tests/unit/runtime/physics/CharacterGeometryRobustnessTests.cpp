#include "CharacterWorldTestHelpers.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct Plane final {
            Math::Vec3 normal;
            float offset{};
        };

        /** @brief Exact continuous capsule/plane oracle used through ordinary Character fixed-tick admission/publication. */
        struct GeometryProbe final {
            std::array<Plane, 4> planes{};
            std::uint32_t planeCount{};
            bool reverse{};
            bool malformed{};
            bool truncated{};
            bool cancelled{};
            std::uint32_t calls{};
            std::uint32_t failCall{};
            CharacterWorld *shutdownWorld{};

            /** @brief Produces only exact continuous intersections; lifecycle failure injection stays outside the geometry oracle. */
            CharacterSweepProbeResult CastPlanes(const CharacterSweepProbeRequest &request) const noexcept {
                CharacterSweepProbeResult result;
                for (std::uint32_t index{}; index < planeCount; ++index) {
                    const auto slot = reverse ? planeCount - 1 - index : index;
                    const auto &plane = planes[slot];
                    const float approach = -Math::Dot(plane.normal, request.direction);
                    if (approach <= 0.0F)
                        continue;
                    const float support = request.capsule.radiusMeters +
                                          request.capsule.cylindricalHalfHeightMeters * std::abs(Math::Dot(plane.normal, request.up));
                    const float gap = Math::Dot(plane.normal, request.position) - plane.offset - support;
                    const float distance = std::max(0.0F, gap / approach);
                    if (distance > request.maximumDistanceMeters)
                        continue;
                    result.hits[result.hitCount++] = {.shape = {request.physicsWorld, {slot + 1, 1}},
                                                      .point = request.position + request.direction * distance - plane.normal * support,
                                                      .normal = malformed ? Math::Vec3{} : plane.normal,
                                                      .distanceMeters = distance};
                }
                return result;
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &probe = *static_cast<GeometryProbe *>(context);
                ++probe.calls;
                if (probe.shutdownWorld)
                    probe.shutdownWorld->Shutdown();
                if (probe.calls == probe.failCall)
                    return Result<CharacterSweepProbeResult>::Failure(
                        MakeError(probe.cancelled ? Physics::PhysicsErrors::QueryCancelled : Physics::PhysicsErrors::InvalidState));
                auto result = probe.CastPlanes(request);
                if (probe.truncated && result.hitCount != 0) {
                    std::fill(result.hits.begin() + result.hitCount, result.hits.end(), result.hits[0]);
                    result.hitCount = MaximumCharacterSweepHits;
                    result.truncated = true;
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            /** @brief Two bounded copied hit batches exercise distinct constraints on successive swept path segments. */
            static Result<CharacterSweepProbeResult> ConstraintSweep(void *, const CharacterSweepProbeRequest &request) noexcept {
                CharacterSweepProbeResult result;
                if (request.direction.x > 0) {
                    for (std::uint32_t index{}; index < MaximumCharacterSweepHits; ++index) {
                        const float angle = static_cast<float>(index) * 0.01F;
                        result.hits[index] = {.shape = {request.physicsWorld, {index + 1, 1}},
                                              .point = request.position + request.direction * 0.25F,
                                              .normal = {-std::cos(angle), 0, std::sin(angle)},
                                              .distanceMeters = 0.25F};
                    }
                    result.hitCount = MaximumCharacterSweepHits;
                } else if (request.direction.y > 0) {
                    result.hits[0] = {.shape = {request.physicsWorld, {MaximumCharacterSweepHits + 1, 1}},
                                      .point = request.position + request.direction * 0.25F,
                                      .normal = {0, -1, 0},
                                      .distanceMeters = 0.25F};
                    result.hitCount = 1;
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        [](void *, const CharacterOverlapProbeRequest &) noexcept {
                    return Result<CharacterOverlapProbeResult>::Success({});
                },
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Sweep};
            }
        };

        SpawnedActiveWorld GeometryWorld(const std::uint32_t iterations = 8, const Math::Vec3 gravity = {},
                                         const std::uint32_t contacts = 16) {
            CharacterWorldSettingsDescriptor settings;
            settings.capacities.maximumControllers = 1;
            settings.work.maximumMovementIterations = iterations;
            auto world = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.gravity = gravity;
            descriptor.maximumStepHeightMeters = 0;
            descriptor.maximumContacts = contacts;
            const auto controller = world->CreateController(descriptor).Value();
            const auto captured = world->ControllerDescriptor(controller);
            REQUIRE(captured.HasValue());
            REQUIRE(captured.Value().capsule.radiusMeters == descriptor.capsule.radiusMeters);
            REQUIRE(captured.Value().capsule.cylindricalHalfHeightMeters == descriptor.capsule.cylindricalHalfHeightMeters);
            REQUIRE(captured.Value().maximumContacts == contacts);
            REQUIRE(world->Activate().HasValue());
            REQUIRE(world->State() == CharacterWorldState::Active);
            OverlapProbe overlap;
            REQUIRE(world->SpawnController(controller, overlap.Context(world->Descriptor())).HasValue());
            return {std::move(world), controller};
        }

        CharacterFixedTickInput GeometryTick(SpawnedActiveWorld &host, GeometryProbe &probe, const std::uint64_t tick) {
            auto input = FixedTick(tick);
            input.fixedDelta = Duration::FromNanoseconds(100'000'000);
            input.query = probe.Context(host.world->Descriptor(), tick);
            return input;
        }

        CharacterLocomotionSnapshot MoveGeometry(SpawnedActiveWorld &host, GeometryProbe &probe, const Math::Vec3 velocity,
                                                 const std::uint64_t tick = 1) {
            auto command = Movement(host.controller, tick, tick);
            command.desiredVelocityMetersPerSecond = velocity;
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            auto input = GeometryTick(host, probe, tick);
            const auto result = host.world->AdvanceFixedTick(input);
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            const auto snapshot = host.world->ControllerLocomotionSnapshot(host.controller).Value();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, host.world->ControllerDescriptor(host.controller).Value()).HasValue());
            return snapshot;
        }

        TEST_CASE("Fixed-tick capsule keeps a corner crease through successive canonical constraints",
                  "[physics][character][geometry][corner]") {
            const bool reverse = GENERATE(false, true);
            const auto contacts = GENERATE(1U, 16U);
            auto host = GeometryWorld(8, {}, contacts);
            GeometryProbe probe;
            probe.planes[0] = {{-1, 0, 0}, -1};
            probe.planes[1] = {{0.6F, 0, -0.8F}, -1};
            probe.planeCount = 2;
            probe.reverse = reverse;
            const auto first = MoveGeometry(host, probe, {30, 10, 30});
            REQUIRE(first.transform.position.y == Catch::Approx(1).margin(1.0e-5F));
            REQUIRE(first.transform.position.x <= 0.75F);
            REQUIRE(Math::Dot(probe.planes[1].normal, first.transform.position) >= -0.75F - 1.0e-5F);
            REQUIRE(first.movement.termination == CharacterMovementTermination::Complete);
            REQUIRE(first.movement.contactCount <= contacts);
            const auto second = MoveGeometry(host, probe, {30, 10, 30}, 2);
            REQUIRE(second.transform.position.y == Catch::Approx(2).margin(1.0e-5F));
            REQUIRE(Math::Length(second.movement.achievedVelocityMetersPerSecond) <= Math::Length(Math::Vec3{30, 10, 30}));
        }

        TEST_CASE("Duplicate coplanar seam hits do not snag or alter the fixed-tick endpoint", "[physics][character][geometry][seam]") {
            auto host = GeometryWorld();
            GeometryProbe probe;
            probe.planes[0] = probe.planes[1] = {{-1, 0, 0}, -1};
            probe.planeCount = GENERATE(1U, 2U);
            const auto snapshot = MoveGeometry(host, probe, {20, 0, 20});
            REQUIRE(snapshot.transform.position.x <= 0.75F);
            REQUIRE(snapshot.transform.position.z == Catch::Approx(2).margin(1.0e-5F));
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::Complete);
        }

        TEST_CASE("A one-iteration fixed tick publishes only checked travel and an explicit limit diagnostic",
                  "[physics][character][geometry][capacity]") {
            auto host = GeometryWorld(1);
            GeometryProbe probe;
            probe.planes[0] = {{-1, 0, 0}, -1};
            probe.planeCount = 1;
            const auto snapshot = MoveGeometry(host, probe, {20, 0, 20});
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::IterationLimit);
            REQUIRE(snapshot.transform.position.x < 0.75F);
            REQUIRE(snapshot.transform.position.z < 0.75F);
            REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            REQUIRE(host.world->PublishedTick().completedTick == 1);
            probe.planeCount = 0;
            const auto next = MoveGeometry(host, probe, {0, 0, 20}, 2);
            REQUIRE(next.movement.termination == CharacterMovementTermination::Complete);
            REQUIRE(next.transform.position.z == Catch::Approx(snapshot.transform.position.z + 2));
        }

        TEST_CASE("Constraint storage exhaustion commits only checked travel independently of contact truncation",
                  "[physics][character][geometry][capacity]") {
            auto host = GeometryWorld(8, {}, 1);
            GeometryProbe probe;
            auto command = Movement(host.controller, 1, 1);
            command.desiredVelocityMetersPerSecond = Math::Vec3{20, 20, 0};
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            auto input = GeometryTick(host, probe, 1);
            input.query.sweep = GeometryProbe::ConstraintSweep;
            REQUIRE(host.world->AdvanceFixedTick(input).HasValue());
            const auto snapshot = host.world->ControllerLocomotionSnapshot(host.controller).Value();
            const auto descriptor = host.world->ControllerDescriptor(host.controller).Value();
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::ConstraintLimit);
            REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            REQUIRE(snapshot.transform.position.x == Catch::Approx(0.23F / std::sqrt(2.0F)));
            REQUIRE(snapshot.transform.position.y == Catch::Approx(0.23F / std::sqrt(2.0F) + 0.23F));
            REQUIRE(snapshot.movement.contactCount == 1);
            REQUIRE(snapshot.movement.truncated);
            REQUIRE(host.world->PublishedTick().completedTick == snapshot.tick);
        }

        TEST_CASE("Grazing continuous evidence blocks even below the old normal epsilon", "[physics][character][geometry][boundary]") {
            auto host = GeometryWorld();
            GeometryProbe probe;
            probe.planes[0] = {{-1, 0, 0}, -0.25F};
            probe.planeCount = 1;
            const auto snapshot = MoveGeometry(host, probe, {0.00001F, 0, 100});
            REQUIRE(snapshot.transform.position.x <= 0);
            REQUIRE(snapshot.transform.position.z == Catch::Approx(10));
        }

        TEST_CASE("Fixed-tick gravity shares the conservative iteration stop without accumulating unswept pressure",
                  "[physics][character][geometry][airborne][capacity]") {
            auto host = GeometryWorld(1, {0, -9.81F, 0});
            GeometryProbe probe;
            const auto snapshot = MoveGeometry(host, probe, {10, 0, 0});
            REQUIRE(snapshot.transform.position.x == Catch::Approx(1));
            REQUIRE(snapshot.transform.position.y == 0);
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::IterationLimit);
            REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            const auto falling = MoveGeometry(host, probe, {}, 2);
            REQUIRE(falling.movement.termination == CharacterMovementTermination::Complete);
            REQUIRE(falling.transform.position.y == Catch::Approx(-0.04905F));
        }

        TEST_CASE("An airborne continuation query failure rolls back the whole fixed tick after grounding",
                  "[physics][character][geometry][airborne][cancellation][rollback]") {
            auto host = GeometryWorld(1, {0, -9.81F, 0});
            GeometryProbe probe;
            const auto before = MoveGeometry(host, probe, {10, 0, 0});
            probe.calls = 0;
            probe.failCall = 2;
            probe.cancelled = GENERATE(false, true);
            auto command = Movement(host.controller, 2, 2);
            command.desiredVelocityMetersPerSecond = Math::Vec3{};
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            const auto advanced = host.world->AdvanceFixedTick(GeometryTick(host, probe, 2));
            RequireError(advanced, probe.cancelled ? Physics::PhysicsErrors::QueryCancelled : Physics::PhysicsErrors::InvalidState);
            REQUIRE(probe.calls == 2);
            REQUIRE(host.world->PublishedTick().completedTick == 1);
            const auto after = host.world->ControllerLocomotionSnapshot(host.controller).Value();
            REQUIRE(after.stateRevision == before.stateRevision);
            REQUIRE(after.transform.position == before.transform.position);
            REQUIRE(after.movement.gravityVelocityMetersPerSecond == before.movement.gravityVelocityMetersPerSecond);
        }

        TEST_CASE("Continuous capsule travel stays behind geometry at the admitted fixed-tick speed boundary",
                  "[physics][character][geometry][high-speed][boundary]") {
            const float speed = GENERATE(10.0F, 1279.0F, 1280.0F);
            auto host = GeometryWorld();
            GeometryProbe probe;
            probe.planes[0] = {{-1, 0, 0}, -1};
            probe.planeCount = 1;
            const auto snapshot = MoveGeometry(host, probe, {speed, 0, 0});
            REQUIRE(snapshot.transform.position.x == Catch::Approx(0.73F).margin(1.0e-5F));
            REQUIRE(snapshot.movement.termination == CharacterMovementTermination::Complete);
            REQUIRE(snapshot.movement.contactCount == 1);
        }

        TEST_CASE("Out-of-envelope intent fails before querying and preserves the last publication",
                  "[physics][character][geometry][validation]") {
            auto host = GeometryWorld();
            GeometryProbe probe;
            auto command = Movement(host.controller, 1, 1);
            command.desiredVelocityMetersPerSecond = Math::Vec3{2000, 0, 0};
            REQUIRE(Math::Length(*command.desiredVelocityMetersPerSecond) * 0.1F >
                    host.world->Settings().Values().work.maximumDisplacementMetersPerTick);
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            auto input = GeometryTick(host, probe, 1);
            RequireError(host.world->AdvanceFixedTick(input), CharacterErrors::PlacementInvalid);
            REQUIRE(probe.calls == 0);
            REQUIRE(host.world->PublishedTick().completedTick == 0);
        }

        TEST_CASE("Malformed incomplete failed and retired sweep evidence cannot publish candidate geometry",
                  "[physics][character][geometry][rollback][lifecycle]") {
            auto host = GeometryWorld();
            GeometryProbe probe;
            static_cast<void>(MoveGeometry(host, probe, {}));
            const auto before = host.world->ControllerLocomotionSnapshot(host.controller).Value();
            const auto descriptor = host.world->ControllerDescriptor(host.controller).Value();
            probe.calls = 0;
            probe.planes[0] = {{-1, 0, 0}, -1};
            probe.planeCount = 1;
            SECTION("malformed normal") {
                probe.malformed = true;
            }
            SECTION("truncated evidence") {
                probe.truncated = true;
            }
            SECTION("query failure") {
                probe.failCall = 1;
            }
            SECTION("cancellation after checked travel") {
                probe.failCall = 2;
                probe.cancelled = true;
            }
            SECTION("ground query failure after checked movement") {
                probe.failCall = 3;
            }
            SECTION("shutdown callback") {
                probe.shutdownWorld = host.world.get();
            }
            auto command = Movement(host.controller, 2, 2);
            command.desiredVelocityMetersPerSecond = Math::Vec3{20, 0, 20};
            REQUIRE(host.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(2);
            input.fixedDelta = Duration::FromNanoseconds(100'000'000);
            input.query = probe.Context(host.world->Descriptor(), 2);
            const auto result = host.world->AdvanceFixedTick(input);
            if (probe.cancelled)
                RequireError(result, Physics::PhysicsErrors::QueryCancelled);
            else
                REQUIRE(result.HasError());
            REQUIRE(host.world->PublishedTick().completedTick == 1);
            if (!probe.shutdownWorld) {
                const auto after = host.world->ControllerLocomotionSnapshot(host.controller).Value();
                REQUIRE(after.stateRevision == before.stateRevision);
                REQUIRE(after.transform.position == before.transform.position);
            }
            host.world->Shutdown();
            host.world->Shutdown();
            REQUIRE(ValidateCharacterLocomotionSnapshot(before, descriptor).HasValue());
        }

        TEST_CASE("Replacement Character worlds cannot consume a retired world's capsule-query context",
                  "[physics][character][geometry][rollback][lifecycle]") {
            auto old = GeometryWorld();
            GeometryProbe probe;
            const auto snapshot = MoveGeometry(old, probe, {});
            const auto descriptor = old.world->ControllerDescriptor(old.controller).Value();
            const auto staleContext = probe.Context(old.world->Descriptor(), 1);
            old.world->Shutdown();
            auto replacement = GeometryWorld();
            auto command = Movement(replacement.controller, 1, 1);
            command.desiredVelocityMetersPerSecond = Math::Vec3{20, 0, 20};
            REQUIRE(replacement.world->QueueMovementCommand(command).HasValue());
            auto input = FixedTick(1);
            input.query = staleContext;
            probe.calls = 0;
            RequireError(replacement.world->AdvanceFixedTick(input), CharacterErrors::HandleWorldMismatch);
            REQUIRE(probe.calls == 0);
            REQUIRE(replacement.world->PublishedTick().completedTick == 0);
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasValue());
        }
    }  // namespace
}  // namespace Horo::Character
