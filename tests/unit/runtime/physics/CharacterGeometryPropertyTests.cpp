#include "CharacterFoundationQualificationTestHelpers.h"
#include "CharacterPropertyCorpus.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;
        using PropertyTest::Recipe;

        /** @brief Bounded analytic wall evidence exercises the production Character solver through its public sweep seam. */
        struct Wall final {
            float offset{2};
            unsigned fault{};
            unsigned calls{};

            static Result<CharacterOverlapProbeResult> Clear(void *, const CharacterOverlapProbeRequest &) noexcept {
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Sweep(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &wall = *static_cast<Wall *>(context);
                ++wall.calls;
                if (wall.fault == 1)
                    return Result<CharacterSweepProbeResult>::Failure(MakeError(Physics::PhysicsErrors::QueryCancelled));
                CharacterSweepProbeResult result;
                if (request.direction.x > 0) {
                    const auto distance =
                        std::max(0.0F, (wall.offset - request.capsule.radiusMeters - request.position.x) / request.direction.x);
                    if (distance <= request.maximumDistanceMeters) {
                        result.hitCount = 1;
                        result.hits[0] = {.shape = {request.physicsWorld, {1, 1}},
                                          .point = request.position + request.direction * distance,
                                          .normal = {-1, 0, 0},
                                          .distanceMeters = distance};
                    }
                }
                if (wall.fault == 2)
                    result.hitCount = MaximumCharacterSweepHits + 1;
                if (wall.fault == 3) {
                    result.hitCount = 1;
                    result.hits[0].normal = {};
                }
                return Result<CharacterSweepProbeResult>::Success(result);
            }

            CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &owner, const std::uint64_t tick) {
                return {owner.sceneGeneration,
                        owner.identity,
                        owner.physicsWorld,
                        this,
                        Clear,
                        owner.collisionFilterGeneration,
                        owner.originGeneration,
                        tick,
                        owner.physicsSnapshotRevision,
                        Sweep};
            }
        };

        struct Outcome final {
            Math::Vec3 position;
            Math::Vec3 velocity;
            std::uint32_t contacts{};
            CharacterMovementTermination termination{};
            bool operator==(const Outcome &) const = default;
        };

        /** @brief Evaluates a whole real fixed tick and failed successor; query traversal and work are bounded independently. */
        std::string RunMovement(const Recipe &input, Outcome &outcome, const bool native) {
            auto settings = QualificationTest::QualificationSettings(1);
            auto owner = WorldDescriptor();
            std::unique_ptr<Physics::PhysicsRuntime> runtime;
            std::unique_ptr<Physics::PhysicsWorld> physics;
#if HORO_TEST_PHYSICS_NATIVE
            if (native) {
                auto preparedRuntime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical);
                if (preparedRuntime.HasError())
                    return "canonical runtime unavailable";
                runtime = std::move(preparedRuntime).Value();
                auto prepared = runtime->PrepareWorld(Physics::Test::SmallWorldSettings());
                if (prepared.HasError())
                    return "canonical prepare failed";
                physics = std::move(prepared).Value();
                if (physics->Activate(owner.physicsWorld).HasError())
                    return "canonical activation failed";
            }
#else
            static_cast<void>(native);
#endif
            auto prepared = CharacterWorld::Prepare(owner, settings);
            if (prepared.HasError())
                return "character prepare failed";
            auto world = std::move(prepared).Value();
            auto descriptor = ControllerDescriptor(world->Descriptor());
            descriptor.gravity = {};
            descriptor.maximumStepHeightMeters = 0;
            descriptor.capsule = {0.125F + static_cast<float>(input.words[2] % 8) / 32,
                                  0.25F + static_cast<float>(input.words[3] % 8) / 16};
            descriptor.maximumContacts = 1 + input.words[4] % 4;
            Wall wall{2 + static_cast<float>(input.words[5] % 8) / 4};
#if HORO_TEST_PHYSICS_NATIVE
            if (physics) {
                const auto fixture =
                    physics->CreateQueryFixture({.shape = Physics::PhysicsStaticPlaneShape{{-1, 0, 0}, -wall.offset},
                                                 .layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                                                 .profile = descriptor.collisionProfile,
                                                 .channel = descriptor.queryChannel});
                if (fixture.HasError())
                    return "native fixture rejected";
                if (physics
                        ->AdvanceFixedTick({.simulationTick = 1,
                                            .sceneGeneration = owner.sceneGeneration,
                                            .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasError())
                    return "native fixture publication failed";
                if (world->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasError())
                    return "native snapshot refresh rejected";
            }
#endif
            const auto created = world->CreateController(descriptor);
            if (created.HasError() || world->Activate().HasError())
                return "generated controller rejected";
            const auto handle = created.Value();
            std::optional<CharacterPhysicsQueryAdapter> adapter;
            if (physics)
                adapter.emplace(*physics);
            const auto query = [&](const std::uint64_t tick) {
                const auto &current = world->Descriptor();
                if (physics)
                    return adapter->Context({current.sceneGeneration, current.identity, current.physicsWorld,
                                             current.collisionFilterGeneration, current.originGeneration, tick,
                                             current.physicsSnapshotRevision});
                return wall.Context(current, tick);
            };
            if (world->SpawnController(handle, query(0)).HasError())
                return "generated spawn rejected";
            auto request = Movement(handle, 1, 1);
            request.desiredVelocityMetersPerSecond =
                Math::Vec3{10 + static_cast<float>(input.words[6] % 100), 0, static_cast<float>(input.words[7] % 32) - 16};
            auto malformed = request;
            malformed.desiredVelocityMetersPerSecond = Math::Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0};
            const auto rejected = world->QueueMovementCommand(malformed);
            if (rejected.HasValue() || rejected.ErrorValue().code.Value() != CharacterErrors::RequestInvalid.code.Value() ||
                world->PublishedTick().completedTick != 0)
                return "malformed admission changed initial state";
            if (world->QueueMovementCommand(request).HasError())
                return "valid movement admission rejected";
            auto tick = FixedTick(1);
            tick.fixedDelta = Duration::FromNanoseconds(100'000'000);
            tick.query = query(1);
            CharacterMetricCapture capture;
            tick.metrics = &capture;
            if (world->AdvanceFixedTick(tick).HasError())
                return "valid movement tick rejected";
            const auto copied = world->ControllerLocomotionSnapshot(handle);
            if (copied.HasError())
                return "movement publication unavailable";
            const auto before = copied.Value();
            if (ValidateCharacterLocomotionSnapshot(before, descriptor).HasError())
                return "published snapshot invalid";
            if (!Math::IsFinite(before.transform.position) ||
                before.transform.position.x > wall.offset - descriptor.capsule.radiusMeters + 1.0e-4F)
                return "capsule crossed wall or nonfinite publication";
            if (std::abs(before.transform.position.z - request.desiredVelocityMetersPerSecond->z * 0.1F) > 1.0e-4F)
                return "wall destroyed tangential travel";
            if (before.controller != handle || before.tick != 1 || before.movement.sequence != 1 ||
                before.movement.contactCount > descriptor.maximumContacts ||
                capture.snapshot.queries > settings.Values().work.maximumQueriesPerTick ||
                capture.snapshot.movementIterations > settings.Values().work.maximumMovementIterations)
                return "movement identity or work bound violated";
            outcome = {before.transform.position, before.movement.achievedVelocityMetersPerSecond, before.movement.contactCount,
                       before.movement.termination};
            request.tick = 2;
            request.sequence = 2;
            const bool extremeIntent = input.words[0] % 5 == 4;
            if (extremeIntent)
                request.desiredVelocityMetersPerSecond = Math::Vec3{std::numeric_limits<float>::max(), 0, 0};
            if (world->QueueMovementCommand(request).HasError())
                return "successor admission rejected";
            tick.tick = 2;
            tick.query = query(2);
            // The native boundary is tested with a stale exact revision; malformed native collectors are never fabricated.
            if (!extremeIntent && (physics || input.words[0] % 4 == 0))
                ++tick.query.physicsSnapshotRevision;
            else if (!extremeIntent)
                wall.fault = 1 + input.words[0] % 3;
            if (world->AdvanceFixedTick(tick).HasValue())
                return "unsafe successor committed";
            const auto after = world->ControllerLocomotionSnapshot(handle);
            if (after.HasError() || world->PublishedTick().completedTick != 1 || after.Value().stateRevision != before.stateRevision ||
                after.Value().transform.position != before.transform.position ||
                after.Value().movement.achievedVelocityMetersPerSecond != before.movement.achievedVelocityMetersPerSecond)
                return "failed tick mutated committed state";
            world->Shutdown();
            if (ValidateCharacterLocomotionSnapshot(before, descriptor).HasError())
                return "retired owned snapshot invalid";
            return {};
        }

        std::string MovementProperty(const Recipe &input, const bool native) {
            Outcome first, second;
            if (const auto failure = RunMovement(input, first, native); !failure.empty())
                return failure;
            if (const auto failure = RunMovement(input, second, native); !failure.empty())
                return failure;
            return first == second ? std::string{} : "identical replay changed movement evidence";
        }
    }  // namespace

    TEST_CASE("Generated Character geometry preserves swept bounds replay and whole tick rollback",
              "[physics][character][character-property][geometry][fuzz]") {
        PropertyTest::Corpus("capsule-wall", [](const Recipe &input) {
            return MovementProperty(input, false);
        });
    }
#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Generated Character geometry uses real Canonical capsule queries",
              "[physics][character][character-property][native][fuzz]") {
        PropertyTest::Corpus("canonical-capsule-wall", [](const Recipe &input) {
            return MovementProperty(input, true);
        });
    }
#endif
}  // namespace Horo::Character
