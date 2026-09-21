#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"

#include <barrier>
#include <catch2/catch_approx.hpp>
#include <cmath>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct MovementResolver final {
            CharacterMovementResult templateResult;
            Math::Vec3 observedPrevious{};
            bool invalidIdentity{};
            bool outOfBounds{};
            bool invalidUp{};
            float excessiveDisplacementMeters{};

            static Result<CharacterMovementResult> Resolve(void *context, const CharacterMovementRequest &request,
                                                           const CharacterTransformPublication &previous) noexcept {
                auto &resolver = *static_cast<MovementResolver *>(context);
                resolver.observedPrevious = previous.position;
                CharacterMovementResult result = resolver.templateResult;
                result.controller = resolver.invalidIdentity ? CharacterControllerHandle{} : request.controller;
                result.tick = request.tick;
                result.sequence = request.sequence;
                result.finalPosition =
                    resolver.outOfBounds
                        ? Math::Vec3{Physics::MaximumPhysicsLocalHalfExtentMeters + 1.0F, 0, 0}
                        : previous.position +
                              Math::Vec3{resolver.excessiveDisplacementMeters > 0.0F ? resolver.excessiveDisplacementMeters : 1.0F, 0, 0};
                result.finalHeading = previous.heading;
                result.up = resolver.invalidUp ? Math::Vec3{0, 0, 1} : previous.up;
                return Result<CharacterMovementResult>::Success(std::move(result));
            }
        };

        [[nodiscard]] MovementResolver GroundedMovementResolver(const CharacterWorldDescriptor &world) {
            MovementResolver resolver;
            resolver.templateResult.grounded = true;
            resolver.templateResult.groundMaterial = Material();
            resolver.templateResult.groundBody = Physics::BodyHandle{world.physicsWorld, {7, 2}};
            resolver.templateResult.groundShape = Physics::ShapeHandle{world.physicsWorld, {8, 3}};
            resolver.templateResult.groundDistanceMeters = 0.02F;
            resolver.templateResult.collisions = CharacterCollisionFlags::Ground | CharacterCollisionFlags::Sides;
            resolver.templateResult.contacts[0].body = Physics::BodyHandle{world.physicsWorld, {7, 2}};
            resolver.templateResult.contacts[0].shape = Physics::ShapeHandle{world.physicsWorld, {8, 3}};
            resolver.templateResult.contacts[0].material = Material();
            resolver.templateResult.contacts[0].penetrationDepthMeters = 0.1F;
            resolver.templateResult.contactCount = 1;
            return resolver;
        }

        struct SweepProbe final {
            std::array<CharacterSweepHit, 4> configuredHits{};
            std::uint32_t configuredHitCount{};
            std::uint32_t calls{};
            bool reverse{};

            static Result<CharacterOverlapProbeResult> NoopOverlap(void *, const CharacterOverlapProbeRequest &) noexcept {
                return Result<CharacterOverlapProbeResult>::Success({});
            }

            static Result<CharacterSweepProbeResult> Run(void *context, const CharacterSweepProbeRequest &) noexcept {
                auto &probe = *static_cast<SweepProbe *>(context);
                ++probe.calls;
                CharacterSweepProbeResult result;
                result.hitCount = probe.configuredHitCount;
                for (std::uint32_t index{}; index < result.hitCount; ++index) {
                    const std::uint32_t source = probe.reverse ? result.hitCount - index - 1 : index;
                    result.hits[index] = probe.configuredHits[source];
                }
                return Result<CharacterSweepProbeResult>::Success(std::move(result));
            }

            [[nodiscard]] CharacterPhysicsQueryContext Context(const CharacterWorldDescriptor &world, const std::uint64_t tick) noexcept {
                return {world.sceneGeneration,
                        world.identity,
                        world.physicsWorld,
                        this,
                        NoopOverlap,
                        world.collisionFilterGeneration,
                        world.originGeneration,
                        tick,
                        world.physicsSnapshotRevision,
                        Run};
            }
        };

        [[nodiscard]] CharacterSweepHit SweepHit(const CharacterWorldDescriptor &world, const std::uint32_t shapeIndex,
                                                 const Math::Vec3 normal, const float distance) {
            return {.body = Physics::BodyHandle{world.physicsWorld, {shapeIndex, 2}},
                    .shape = Physics::ShapeHandle{world.physicsWorld, {shapeIndex, 3}},
                    .point = {},
                    .normal = normal,
                    .material = std::nullopt,
                    .response = Physics::PhysicsQueryResponse::Block,
                    .distanceMeters = distance};
        }

        enum class InvalidMovementEvidence {
            OutOfBounds,
            ExcessiveDisplacement,
            InvalidUp,
        };

        void RequireRejectedMovementResult(const InvalidMovementEvidence evidence) {
            auto spawned = SpawnedActiveWorldWithController();
            auto resolver = GroundedMovementResolver(spawned.world->Descriptor());
            switch (evidence) {
                case InvalidMovementEvidence::OutOfBounds:
                    resolver.outOfBounds = true;
                    break;
                case InvalidMovementEvidence::ExcessiveDisplacement:
                    resolver.excessiveDisplacementMeters = spawned.world->Settings().Values().work.maximumDisplacementMetersPerTick + 1.0F;
                    break;
                case InvalidMovementEvidence::InvalidUp:
                    resolver.invalidUp = true;
                    break;
            }
            const CharacterTickObserver observer{.context = &resolver, .movementResult = MovementResolver::Resolve};
            REQUIRE(spawned.world->QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
            RequireError(spawned.world->AdvanceFixedTick(FixedTick(1, observer)), CharacterErrors::PlacementInvalid);
            REQUIRE((spawned.world->PublishedTick() == CharacterPublishedTick{}));
            RequireError(spawned.world->ControllerLocomotionSnapshot(spawned.controller), CharacterErrors::InvalidState);
        }

        TEST_CASE("Character publishes complete immutable locomotion state and authoritative transforms",
                  "[physics][character][world][snapshot]") {
            auto spawned = SpawnedActiveWorldWithController();
            const auto controller = spawned.controller;
            auto &world = *spawned.world;
            RequireError(world.ControllerLocomotionSnapshot(controller), CharacterErrors::InvalidState);

            auto resolver = GroundedMovementResolver(world.Descriptor());
            const CharacterTickObserver observer{.context = &resolver, .movementResult = MovementResolver::Resolve};
            auto request = Movement(controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{3, 0, 0};
            REQUIRE(world.QueueMovementCommand(request).HasValue());
            REQUIRE(world.AdvanceFixedTick(FixedTick(1, observer)).HasValue());

            const auto snapshot = world.ControllerLocomotionSnapshot(controller);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().controller == controller);
            REQUIRE(snapshot.Value().movement.controller == controller);
            REQUIRE(snapshot.Value().transform.controller == controller);
            REQUIRE(snapshot.Value().tick == 1);
            REQUIRE(snapshot.Value().stateRevision == 1);
            REQUIRE(snapshot.Value().movement.grounded);
            REQUIRE(snapshot.Value().movement.platformAttached == false);
            REQUIRE(snapshot.Value().movement.collisions == (CharacterCollisionFlags::Ground | CharacterCollisionFlags::Sides));
            REQUIRE(snapshot.Value().movement.contactCount == 1);
            REQUIRE(snapshot.Value().movement.contacts[0].body.has_value());
            REQUIRE(snapshot.Value().movement.contacts[0].shape.world == world.Descriptor().physicsWorld);
            REQUIRE(snapshot.Value().transform.authority == CharacterTransformAuthority::CharacterController);
            REQUIRE(snapshot.Value().transform.position == Math::Vec3{1, 0, 0});
            REQUIRE(snapshot.Value().transform.grounded);
            REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot.Value(), world.ControllerDescriptor(controller).Value()).HasValue());

            auto detachedCopy = world.ControllerTransform(controller).Value();
            detachedCopy.position = {100, 100, 100};
            detachedCopy.grounded = false;
            request = Movement(controller, 2, 2);
            REQUIRE(world.QueueMovementCommand(request).HasValue());
            REQUIRE(world.AdvanceFixedTick(FixedTick(2, observer)).HasValue());
            const auto next = world.ControllerLocomotionSnapshot(controller);
            REQUIRE(next.HasValue());
            REQUIRE(resolver.observedPrevious == Math::Vec3{1, 0, 0});
            REQUIRE(next.Value().transform.position == Math::Vec3{2, 0, 0});
            REQUIRE(next.Value().stateRevision == 2);
        }

        TEST_CASE("Character publishes the backend-free baseline movement and heading intent",
                  "[physics][character][world][snapshot][baseline]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto &world = *spawned.world;
            auto request = Movement(spawned.controller, 1, 1);
            const Math::Vec3 velocity{3, 0, 0};
            const auto heading = Math::Quaternion::FromAxisAngle({0, 1, 0}, Math::Pi / 2.0F);
            request.desiredVelocityMetersPerSecond = velocity;
            request.desiredHeading = heading;
            const auto input = FixedTick(1);
            REQUIRE(world.QueueMovementCommand(request).HasValue());
            REQUIRE(world.AdvanceFixedTick(input).HasValue());

            const auto snapshot = world.ControllerLocomotionSnapshot(spawned.controller);
            REQUIRE(snapshot.HasValue());
            const auto &movement = snapshot.Value().movement;
            const auto &transform = snapshot.Value().transform;
            const auto expectedPosition =
                velocity * static_cast<float>(static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0);
            REQUIRE(movement.controller == spawned.controller);
            REQUIRE(movement.tick == 1);
            REQUIRE(movement.sequence == 1);
            REQUIRE(movement.finalPosition == expectedPosition);
            REQUIRE(movement.finalHeading == heading);
            REQUIRE(movement.achievedVelocityMetersPerSecond == velocity);
            REQUIRE(movement.up == Math::Vec3{0, 1, 0});
            REQUIRE(movement.groundNormal == Math::Vec3{0, 1, 0});
            REQUIRE_FALSE(movement.grounded);
            REQUIRE_FALSE(movement.platformAttached);
            REQUIRE(movement.groundingRevalidationRequired);
            REQUIRE(transform.position == expectedPosition);
            REQUIRE(transform.heading == heading);
            REQUIRE(transform.groundingRevalidationRequired);
            REQUIRE(transform.authority == CharacterTransformAuthority::CharacterController);
        }

        TEST_CASE("Character capsule sweep advances to the skin boundary and retains collision evidence",
                  "[physics][character][world][sweep][slide]") {
            auto spawned = SpawnedActiveWorldWithController();
            SweepProbe probe;
            probe.configuredHits[0] = SweepHit(spawned.world->Descriptor(), 7, {-1, 0, 0}, 0.05F);
            probe.configuredHitCount = 1;

            auto request = Movement(spawned.controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 0};
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());

            const auto snapshot = spawned.world->ControllerLocomotionSnapshot(spawned.controller);
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().movement.finalPosition.x == Catch::Approx(0.03F).margin(1.0e-5F));
            REQUIRE(snapshot.Value().movement.achievedVelocityMetersPerSecond.x == Catch::Approx(1.8F).margin(1.0e-4F));
            REQUIRE(snapshot.Value().movement.collisions == CharacterCollisionFlags::Sides);
            REQUIRE(snapshot.Value().movement.contactCount == 1);
            REQUIRE(snapshot.Value().movement.contacts[0].normal == Math::Vec3{-1, 0, 0});
            const auto defaultMaterial = spawned.world->ControllerDescriptor(spawned.controller).Value().defaultMaterial;
            REQUIRE(snapshot.Value().movement.contacts[0].material.asset == defaultMaterial.asset);
            REQUIRE(snapshot.Value().movement.contacts[0].material.assetGeneration == defaultMaterial.assetGeneration);
            REQUIRE(snapshot.Value().movement.contacts[0].material.slot == defaultMaterial.slot);
            REQUIRE(spawned.world->TickStatistics().retainedContacts == 1);
            REQUIRE(probe.calls == 2);
        }

        TEST_CASE("Character capsule slide reduction is stable when simultaneous hits change callback order",
                  "[physics][character][world][sweep][determinism]") {
            const auto resolve = [](const bool reverse) {
                auto spawned = SpawnedActiveWorldWithController();
                SweepProbe probe;
                probe.configuredHits[0] = SweepHit(spawned.world->Descriptor(), 7, {-1, 0, 0}, 0.05F);
                probe.configuredHits[1] = SweepHit(spawned.world->Descriptor(), 8, {0, 0, -1}, 0.05F);
                probe.configuredHitCount = 2;
                probe.reverse = reverse;
                auto request = Movement(spawned.controller, 1, 1);
                request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 6};
                auto input = FixedTick(1);
                input.query = probe.Context(spawned.world->Descriptor(), 1);
                REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
                REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
                return spawned.world->ControllerLocomotionSnapshot(spawned.controller).Value();
            };

            const auto forward = resolve(false);
            const auto reversed = resolve(true);
            REQUIRE(forward.movement.finalPosition == reversed.movement.finalPosition);
            REQUIRE(forward.movement.achievedVelocityMetersPerSecond == reversed.movement.achievedVelocityMetersPerSecond);
            REQUIRE(forward.movement.collisions == CharacterCollisionFlags::Sides);
            REQUIRE(forward.movement.contactCount == 2);
            REQUIRE(forward.movement.contacts[0].normal == reversed.movement.contacts[0].normal);
            REQUIRE(forward.movement.contacts[1].normal == reversed.movement.contacts[1].normal);
            REQUIRE(Math::LengthSquared(forward.movement.achievedVelocityMetersPerSecond) <= 72.0F + 1.0e-4F);
        }

        TEST_CASE("Character capsule sweep rejects stale snapshots and malformed hit evidence without publication",
                  "[physics][character][world][sweep][validation]") {
            auto spawned = SpawnedActiveWorldWithController();
            SweepProbe probe;
            probe.configuredHits[0] = SweepHit(spawned.world->Descriptor(), 7, {}, 0.05F);
            probe.configuredHitCount = 1;
            auto request = Movement(spawned.controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 0};
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 2);
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::QuerySnapshotStale);
            REQUIRE((spawned.world->PublishedTick() == CharacterPublishedTick{}));

            request = Movement(spawned.controller, 2, 2);
            request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 0};
            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            input = FixedTick(2);
            input.query = probe.Context(spawned.world->Descriptor(), 2);
            RequireError(spawned.world->AdvanceFixedTick(input), CharacterErrors::DescriptorInvalid);
            REQUIRE((spawned.world->PublishedTick() == CharacterPublishedTick{}));
        }

        TEST_CASE("Character rejects malformed movement evidence without publishing and shuts down terminally",
                  "[physics][character][world][snapshot][lifecycle]") {
            auto spawned = SpawnedActiveWorldWithController();
            auto &world = *spawned.world;
            auto resolver = GroundedMovementResolver(world.Descriptor());
            resolver.invalidIdentity = true;
            const CharacterTickObserver observer{.context = &resolver, .movementResult = MovementResolver::Resolve};
            REQUIRE(world.QueueMovementCommand(Movement(spawned.controller, 1, 1)).HasValue());
            RequireError(world.AdvanceFixedTick(FixedTick(1, observer)), CharacterErrors::RequestInvalid);
            REQUIRE((world.PublishedTick() == CharacterPublishedTick{}));
            REQUIRE(world.ControllerTransform(spawned.controller).Value().position == Math::Vec3{});
            RequireError(world.ControllerLocomotionSnapshot(spawned.controller), CharacterErrors::InvalidState);

            world.Shutdown();
            REQUIRE(world.State() == CharacterWorldState::Destroyed);
            RequireError(world.ControllerTransform(spawned.controller), CharacterErrors::InvalidState);
            RequireError(world.ControllerLocomotionSnapshot(spawned.controller), CharacterErrors::InvalidState);
            world.Shutdown();
        }

        TEST_CASE("Character rejects movement results outside the Physics local-origin envelope",
                  "[physics][character][world][snapshot][validation]") {
            RequireRejectedMovementResult(InvalidMovementEvidence::OutOfBounds);
        }

        TEST_CASE("Character rejects movement results beyond the fixed-tick displacement bound",
                  "[physics][character][world][snapshot][validation]") {
            RequireRejectedMovementResult(InvalidMovementEvidence::ExcessiveDisplacement);
        }

        TEST_CASE("Character rejects movement results that change the controller up axis",
                  "[physics][character][world][snapshot][validation]") {
            RequireRejectedMovementResult(InvalidMovementEvidence::InvalidUp);
        }

        TEST_CASE("Character steady-state command movement does not allocate after preparation",
                  "[physics][character][world][command][allocation]") {
            auto [world, controllers] = ActiveWorldWithControllers();
            const auto before = Tests::AllocationProbe::Count();

            REQUIRE(world->QueueMovementCommand(Movement(controllers.front(), 1, 1)).HasValue());
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE(Tests::AllocationProbe::Count() == before);
        }

        TEST_CASE("Character capsule sweep resolution does not allocate after preparation",
                  "[physics][character][world][sweep][allocation]") {
            auto spawned = SpawnedActiveWorldWithController();
            SweepProbe probe;
            probe.configuredHits[0] = SweepHit(spawned.world->Descriptor(), 7, {-1, 0, 0}, 0.05F);
            probe.configuredHitCount = 1;
            auto request = Movement(spawned.controller, 1, 1);
            request.desiredVelocityMetersPerSecond = Math::Vec3{6, 0, 0};
            auto input = FixedTick(1);
            input.query = probe.Context(spawned.world->Descriptor(), 1);
            const auto before = Tests::AllocationProbe::Count();

            REQUIRE(spawned.world->QueueMovementCommand(request).HasValue());
            REQUIRE(spawned.world->AdvanceFixedTick(input).HasValue());
            REQUIRE(Tests::AllocationProbe::Count() == before);
        }

        TEST_CASE("Character fixed ticks canonically order commands and select the final replacement",
                  "[physics][character][world][command]") {
            auto [world, controllers] = ActiveWorldWithControllers(2);
            const auto [first, second] = controllers;

            REQUIRE(world->QueueMovementCommand(Movement(second, 1, 5)).Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 2)).Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 3)).Value().status == CharacterCommandAdmissionStatus::Deferred);

            CommandTrace trace;
            REQUIRE(world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
            REQUIRE(trace.phaseCount == 3);
            REQUIRE(trace.phases == std::array{CharacterTickPhase::FreezeCommands, CharacterTickPhase::ResolveMovement,
                                               CharacterTickPhase::PublishCompletedTick});
            REQUIRE(trace.movementCount == 2);
            REQUIRE(trace.movements[0].controller == first);
            REQUIRE(trace.movements[0].sequence == 3);
            REQUIRE(trace.movements[1].controller == second);
            REQUIRE(trace.movements[1].sequence == 5);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{1, 1, 2}));

            RequireError(world->QueueMovementCommand(Movement(first, 2, 2)), CharacterErrors::CommandOrderInvalid);
            trace = {};
            REQUIRE(world->AdvanceFixedTick(FixedTick(2, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 0);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{2, 2, 0}));
            RequireError(world->QueueMovementCommand(Movement(first, 2, 6)), CharacterErrors::CommandOrderInvalid);

            const auto statistics = world->TickStatistics();
            REQUIRE(statistics.completedTicks == 2);
            REQUIRE(statistics.admittedCommands == 3);
            REQUIRE(statistics.rejectedCommands == 2);
            REQUIRE(statistics.pendingCommands == 0);
            REQUIRE(statistics.maximumCommandDepth == 3);
        }

        TEST_CASE("Character fixed ticks reject foreign owner threads without closing the command frame",
                  "[physics][character][world][command][thread]") {
            auto [world, controllers] = ActiveWorldWithControllers();
            const auto handle = controllers.front();
            REQUIRE(world->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());

            std::optional<Result<void>> tickResult;
            std::thread foreign([&] {
                tickResult = world->AdvanceFixedTick(FixedTick(1));
            });
            foreign.join();
            REQUIRE(tickResult.has_value());
            RequireError(*tickResult, CharacterErrors::InvalidState);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{}));
            REQUIRE(world->TickStatistics().pendingCommands == 1);
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{1, 1, 1}));
        }

        TEST_CASE("Character command admission rejects duplicates and preserves an over-budget tick",
                  "[physics][character][world][command][capacity]") {
            CharacterWorldSettingsDescriptor values;
            values.capacities.maximumControllers = 2;
            values.capacities.maximumQueuedCommands = 2;
            values.work.maximumCommandsPerTick = 1;
            const auto settings = CharacterWorldSettings::Capture(values);
            REQUIRE(settings.HasValue());
            auto prepared = CharacterWorld::Prepare(WorldDescriptor(), settings.Value());
            REQUIRE(prepared.HasValue());
            auto world = std::move(prepared).Value();
            const auto first = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            const auto second = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            REQUIRE(world->QueueMovementCommand(Movement(first, 1, 1)).HasValue());
            RequireError(world->QueueMovementCommand(Movement(first, 1, 1)), CharacterErrors::CommandOrderInvalid);
            REQUIRE(world->QueueMovementCommand(Movement(second, 1, 1)).HasValue());
            REQUIRE(world->QueueMovementCommand(Movement(first, 2, 2)).Value().status == CharacterCommandAdmissionStatus::RejectedFull);
            REQUIRE(world->TickStatistics().commandOverflowCount == 1);
            RequireError(world->AdvanceFixedTick(FixedTick(1)), CharacterErrors::CapacityExceeded);
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{}));
            REQUIRE(world->TickStatistics().pendingCommands == 2);

            RequireError(world->AdvanceFixedTick(FixedTick(2)), CharacterErrors::CommandOrderInvalid);
            auto invalid = FixedTick(1);
            invalid.fixedDelta = {};
            RequireError(world->AdvanceFixedTick(invalid), CharacterErrors::CommandOrderInvalid);
        }

        struct ReentrantProducer final {
            CharacterWorld *world{};
            CharacterMovementRequest closed;
            CharacterMovementRequest future;
            std::optional<Result<CharacterCommandAdmission>> closedResult;
            std::optional<Result<CharacterCommandAdmission>> futureResult;

            static void Movement(void *context, const CharacterMovementRequest &) noexcept {
                auto &producer = *static_cast<ReentrantProducer *>(context);
                producer.closedResult = producer.world->QueueMovementCommand(producer.closed);
                producer.futureResult = producer.world->QueueMovementCommand(producer.future);
            }
        };

        TEST_CASE("Character closes the current command frame before producer callbacks execute",
                  "[physics][character][world][command][thread]") {
            auto [world, controllers] = ActiveWorldWithControllers();
            const auto handle = controllers.front();
            REQUIRE(world->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());

            ReentrantProducer producer{world.get(), Movement(handle, 1, 2), Movement(handle, 2, 2)};
            const CharacterTickObserver observer{.context = &producer, .movement = ReentrantProducer::Movement};
            REQUIRE(world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
            REQUIRE(producer.closedResult.has_value());
            RequireError(*producer.closedResult, CharacterErrors::CommandOrderInvalid);
            REQUIRE(producer.futureResult.has_value());
            REQUIRE(producer.futureResult->Value().status == CharacterCommandAdmissionStatus::Deferred);
            REQUIRE(world->TickStatistics().pendingCommands == 1);
            REQUIRE(world->AdvanceFixedTick(FixedTick(2)).HasValue());
            REQUIRE((world->PublishedTick() == CharacterPublishedTick{2, 2, 1}));
        }

        TEST_CASE("Concurrent Character producers receive bounded non-blocking admission outcomes",
                  "[physics][character][world][command][thread]") {
            constexpr std::size_t ProducerCount = 16;
            auto world = PreparedWorld(1);
            const auto handle = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
            REQUIRE(world->Activate().HasValue());

            std::barrier start{static_cast<std::ptrdiff_t>(ProducerCount + 1)};
            std::array<std::optional<Result<CharacterCommandAdmission>>, ProducerCount> results;
            std::vector<std::thread> producers;
            producers.reserve(ProducerCount);
            for (std::size_t index = 0; index < ProducerCount; ++index) {
                producers.emplace_back([&, index] {
                    start.arrive_and_wait();
                    results[index] = world->QueueMovementCommand(Movement(handle, 1, index + 1));
                });
            }
            start.arrive_and_wait();
            for (std::thread &producer : producers)
                producer.join();

            std::uint32_t deferred{};
            for (const auto &result : results) {
                REQUIRE(result.has_value());
                REQUIRE(result->HasValue());
                const auto status = result->Value().status;
                REQUIRE((status == CharacterCommandAdmissionStatus::Deferred || status == CharacterCommandAdmissionStatus::RejectedBusy));
                deferred += static_cast<std::uint32_t>(status == CharacterCommandAdmissionStatus::Deferred);
            }
            REQUIRE(deferred != 0);
            REQUIRE(world->AdvanceFixedTick(FixedTick(1)).HasValue());
            REQUIRE(world->PublishedTick().appliedCommands == 1);
            REQUIRE(world->TickStatistics().pendingCommands == 0);
        }

        struct TrackedRecord final {
            explicit TrackedRecord(std::uint32_t &live) noexcept : live_(&live) {
                ++*live_;
            }

            TrackedRecord(const TrackedRecord &) = delete;
            TrackedRecord &operator=(const TrackedRecord &) = delete;

            TrackedRecord(TrackedRecord &&other) noexcept : live_(std::exchange(other.live_, nullptr)) {}

            TrackedRecord &operator=(TrackedRecord &&) noexcept = delete;

            ~TrackedRecord() noexcept {
                Release();
            }

        private:
            void Release() noexcept {
                if (live_ != nullptr)
                    --*live_;
                live_ = nullptr;
            }

            std::uint32_t *live_{};
        };

        TEST_CASE("Character registry drains owned records and retires non-wrapping generations",
                  "[physics][character][world][lifecycle]") {
            using Registry = Detail::CharacterControllerRegistry<TrackedRecord>;
            std::uint32_t live{};
            Registry source{61, WorldId(), {.maximumSlots = 1, .maximumGeneration = 2}};
            auto registry = std::move(source);
            RequireError(source.Acquire(TrackedRecord{live}), CharacterErrors::CapacityExceeded);
            const auto first = registry.Acquire(TrackedRecord{live});
            REQUIRE(first.HasValue());
            REQUIRE(live == 1);
            REQUIRE(registry.Remove(first.Value()).HasValue());
            REQUIRE(live == 0);
            const auto last = registry.Acquire(TrackedRecord{live});
            REQUIRE(last.HasValue());
            REQUIRE(last.Value().slot.generation == 2);
            REQUIRE(registry.Remove(last.Value()).HasValue());
            REQUIRE(live == 0);
            RequireError(registry.Acquire(TrackedRecord{live}), CharacterErrors::GenerationExhausted);
            REQUIRE(live == 0);

            Registry drainable{61, WorldId(), {.maximumSlots = 1}};
            REQUIRE(drainable.Acquire(TrackedRecord{live}).HasValue());
            REQUIRE(live == 1);
            drainable.Drain();
            REQUIRE(live == 0);
            REQUIRE(drainable.Statistics().active == 0);
        }

    }  // namespace
}  // namespace Horo::Character
