#include "AllocationProbe.h"
#include "CharacterWorldTestHelpers.h"

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        CharacterDebugCaptureRequest CaptureRequest(const CharacterWorld &world, const CharacterControllerHandle controller) {
            const auto &descriptor = world.Descriptor();
            return {controller, descriptor.physicsWorld, descriptor.collisionFilterGeneration, descriptor.originGeneration};
        }

        struct CaptureDuringOverlap final {
            CharacterWorld *world{};
            CharacterDebugCaptureRequest request;
            CharacterDebugCaptureStatus observed{};
            bool blocked{};
            std::uint32_t calls{};

            static Result<CharacterOverlapProbeResult> Run(void *context, const CharacterOverlapProbeRequest &) noexcept {
                auto &probe = *static_cast<CaptureDuringOverlap *>(context);
                ++probe.calls;
                probe.observed = probe.world->CaptureDebugSnapshot(probe.request).status;
                return Result<CharacterOverlapProbeResult>::Success({probe.blocked ? 1U : 0U, {0, 1, 0}});
            }
        };

        struct TraceSweep final {
            std::array<CharacterSweepProbeRequest, MaximumCharacterDebugProbes> requests{};
            std::uint32_t calls{};
            bool malformed{};

            static Result<CharacterSweepProbeResult> Run(void *context, const CharacterSweepProbeRequest &request) noexcept {
                auto &producer = *static_cast<TraceSweep *>(context);
                if (producer.calls < producer.requests.size())
                    producer.requests[producer.calls] = request;
                ++producer.calls;
                CharacterSweepProbeResult result;
                if (producer.malformed)
                    result.hitCount = MaximumCharacterSweepHits + 1;
                return Result<CharacterSweepProbeResult>::Success(result);
            }
        };
    }  // namespace

    TEST_CASE("Character debug capture owns actual spawn probes without invoking the producer", "[character][debug-snapshot]") {
        auto active = ActiveWorldWithControllers();
        const auto controller = active.controllers[0];
        const auto request = CaptureRequest(*active.world, controller);
        REQUIRE(active.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::NoPublication);
        OverlapProbe producer;
        producer.overlappingCalls = 1;
        REQUIRE(active.world->SpawnController(controller, producer.Context(active.world->Descriptor())).HasValue());
        const auto capture = active.world->CaptureDebugSnapshot(request);
        REQUIRE(capture.status == CharacterDebugCaptureStatus::Captured);
        REQUIRE(capture.snapshot.has_value());
        const auto &snapshot = *capture.snapshot;
        REQUIRE(snapshot.Probes().size() == 2);
        REQUIRE(snapshot.ProbeAvailability() == CharacterDebugProbeAvailability::Complete);
        REQUIRE_FALSE(snapshot.Locomotion().has_value());
        REQUIRE(snapshot.Identity().physicsSnapshotRevision == active.world->Descriptor().physicsSnapshotRevision);
        REQUIRE(snapshot.Probes()[0].purpose == CharacterDebugProbePurpose::SpawnRecovery);
        REQUIRE(snapshot.Probes()[0].reportedHits == 1);
        REQUIRE(snapshot.Probes()[1].reportedHits == 0);
        REQUIRE(snapshot.Probes()[1].position.y == producer.positions[1].y);
        REQUIRE(producer.calls == 2);
        active.world->Shutdown();
        REQUIRE(active.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::Retired);
        REQUIRE(snapshot.Probes().size() == 2);
        REQUIRE(snapshot.Transform().position.y == producer.positions[1].y);
    }

    TEST_CASE("Character debug consumer capacities do not mutate source publications", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        auto request = CaptureRequest(*host.world, host.controller);
        request.maximumProbes = 0;
        const auto limited = host.world->CaptureDebugSnapshot(request);
        REQUIRE(limited.status == CharacterDebugCaptureStatus::CapacityLimited);
        REQUIRE(limited.snapshot->Probes().empty());
        REQUIRE(limited.snapshot->OmittedProbes() == 1);
        REQUIRE(limited.snapshot->ProbeAvailability() == CharacterDebugProbeAvailability::CapacityLimited);
        request.maximumProbes = MaximumCharacterDebugProbes;
        const auto complete = host.world->CaptureDebugSnapshot(request);
        REQUIRE(complete.status == CharacterDebugCaptureStatus::Captured);
        REQUIRE(complete.snapshot->Probes().size() == 1);
        REQUIRE(complete.snapshot->Identity().transformPublicationRevision == limited.snapshot->Identity().transformPublicationRevision);
        request.maximumProbes = MaximumCharacterDebugProbes + 1;
        REQUIRE(host.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::InvalidRequest);
    }

    TEST_CASE("Character debug capture rejects foreign stale and off-owner requests", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        auto request = CaptureRequest(*host.world, host.controller);
        const auto valid = request;
        ++request.originGeneration;
        REQUIRE(host.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::ForeignGeneration);
        request = valid;
        ++request.controller.slot.generation;
        REQUIRE(host.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::StaleController);
        CharacterDebugCaptureStatus status{};
        std::thread reader([&host, &valid, &status] {
            status = host.world->CaptureDebugSnapshot(valid).status;
        });
        reader.join();
        REQUIRE(status == CharacterDebugCaptureStatus::WrongThread);
        REQUIRE(host.world->CaptureDebugSnapshot(valid).status == CharacterDebugCaptureStatus::Captured);
        auto replacement = SpawnedActiveWorldWithController();
        REQUIRE(replacement.world->CaptureDebugSnapshot(valid).status == CharacterDebugCaptureStatus::ForeignGeneration);
    }

    TEST_CASE("Character debug capture cannot observe or adopt failed placement evidence", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        const auto request = CaptureRequest(*host.world, host.controller);
        const auto before = host.world->CaptureDebugSnapshot(request);
        CaptureDuringOverlap producer{host.world.get(), request, {}, true};
        auto query = OverlapProbe{}.Context(host.world->Descriptor(), 1);
        query.context = &producer;
        query.overlap = CaptureDuringOverlap::Run;
        CharacterTeleportRequest teleport{host.controller, 1, {0, 2, 0}};
        REQUIRE(host.world->TeleportController(teleport, query).HasError());
        REQUIRE(producer.observed == CharacterDebugCaptureStatus::Busy);
        const auto after = host.world->CaptureDebugSnapshot(request);
        REQUIRE(after.status == CharacterDebugCaptureStatus::Captured);
        REQUIRE(after.snapshot->Identity().transformPublicationRevision == before.snapshot->Identity().transformPublicationRevision);
        REQUIRE(after.snapshot->Probes()[0].purpose == CharacterDebugProbePurpose::SpawnRecovery);
        REQUIRE(after.snapshot->Probes()[0].position.y == before.snapshot->Probes()[0].position.y);
        REQUIRE(producer.calls == 1);
    }

    TEST_CASE("Character debug movement probes copy real requests and survive malformed-tick rollback", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        const auto request = CaptureRequest(*host.world, host.controller);
        TraceSweep producer;
        OverlapProbe overlap;
        auto input = FixedTick(1);
        input.query = overlap.Context(host.world->Descriptor(), 1);
        input.query.context = &producer;
        input.query.sweep = TraceSweep::Run;
        // This command changes neither shape nor stance, so no overlap callback is required during movement.
        input.query.overlap = nullptr;
        auto command = Movement(host.controller, 1, 1);
        command.desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0};
        REQUIRE(host.world->QueueMovementCommand(command).HasValue());
        REQUIRE(host.world->AdvanceFixedTick(input).HasValue());
        const auto before = host.world->CaptureDebugSnapshot(request);
        REQUIRE(before.status == CharacterDebugCaptureStatus::Captured);
        REQUIRE(before.snapshot->Locomotion().has_value());
        REQUIRE(before.snapshot->Probes().size() == producer.calls);
        REQUIRE(producer.calls > 0);
        for (std::uint32_t index{}; index < producer.calls; ++index) {
            const auto &probe = before.snapshot->Probes()[index];
            const auto &actual = producer.requests[index];
            REQUIRE(probe.kind == CharacterDebugProbeKind::Sweep);
            REQUIRE(probe.position.x == actual.position.x);
            REQUIRE(probe.position.y == actual.position.y);
            REQUIRE(probe.direction.x == actual.direction.x);
            REQUIRE(probe.direction.y == actual.direction.y);
            REQUIRE(probe.distanceMeters == actual.maximumDistanceMeters);
            REQUIRE(probe.iteration == actual.iteration);
        }
        const auto callsBeforeCapture = producer.calls;
        REQUIRE(host.world->CaptureDebugSnapshot(request).snapshot.has_value());
        REQUIRE(producer.calls == callsBeforeCapture);
        producer.malformed = true;
        command.tick = 2;
        command.sequence = 2;
        input.tick = 2;
        input.query.tick = 2;
        REQUIRE(host.world->QueueMovementCommand(command).HasValue());
        REQUIRE(host.world->AdvanceFixedTick(input).HasError());
        const auto after = host.world->CaptureDebugSnapshot(request);
        REQUIRE(after.snapshot->Identity().transformPublicationRevision == before.snapshot->Identity().transformPublicationRevision);
        REQUIRE(after.snapshot->Identity().sourceTick == 1);
        REQUIRE(after.snapshot->Probes().size() == before.snapshot->Probes().size());
        REQUIRE(after.snapshot->Probes()[0].distanceMeters == before.snapshot->Probes()[0].distanceMeters);
    }

    TEST_CASE("Character debug age measures completed ticks without inventing a new source publication", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        auto request = CaptureRequest(*host.world, host.controller);
        REQUIRE(host.world->AdvanceFixedTick(FixedTick(1)).HasValue());
        const auto aged = host.world->CaptureDebugSnapshot(request);
        REQUIRE(aged.snapshot->Identity().sourceTick == 0);
        REQUIRE(aged.snapshot->Identity().observationTick == 1);
        REQUIRE(aged.snapshot->Identity().ageTicks == 1);
        request.maximumAgeTicks = 0;
        const auto rejected = host.world->CaptureDebugSnapshot(request);
        REQUIRE(rejected.status == CharacterDebugCaptureStatus::AgeExceeded);
        REQUIRE_FALSE(rejected.snapshot.has_value());
        request.maximumAgeTicks = 1;
        REQUIRE(host.world->CaptureDebugSnapshot(request).status == CharacterDebugCaptureStatus::Captured);
    }

    TEST_CASE("Character debug preparation capacity bounds both prefixes without changing recovery", "[character][debug-snapshot]") {
        auto settings = Settings(1).Values();
        settings.capacities.maximumDebugPrimitives = 2;
        const auto captured = CharacterWorldSettings::Capture(settings);
        REQUIRE(captured.HasValue());
        auto world = CharacterWorld::Prepare(WorldDescriptor(), captured.Value()).Value();
        const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
        REQUIRE(world->Activate().HasValue());
        OverlapProbe producer;
        producer.overlappingCalls = 2;
        REQUIRE(world->SpawnController(controller, producer.Context(world->Descriptor())).HasValue());
        const auto capture = world->CaptureDebugSnapshot(CaptureRequest(*world, controller));
        REQUIRE(capture.status == CharacterDebugCaptureStatus::CapacityLimited);
        REQUIRE(capture.snapshot->ProbeRetentionCapacity() == 1);
        REQUIRE(capture.snapshot->Probes().size() == 1);
        REQUIRE(capture.snapshot->OmittedProbes() == 2);
        REQUIRE(capture.snapshot->Transform().position.y == producer.positions[2].y);
        REQUIRE(producer.calls == 3);
    }

    TEST_CASE("Character debug explicitly reports unavailable retention while preserving actual capsule state",
              "[character][debug-snapshot]") {
        auto settings = Settings(1).Values();
        settings.capacities.maximumDebugPrimitives = 1;
        auto world = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value()).Value();
        const auto controller = world->CreateController(ControllerDescriptor(world->Descriptor())).Value();
        REQUIRE(world->Activate().HasValue());
        OverlapProbe producer;
        REQUIRE(world->SpawnController(controller, producer.Context(world->Descriptor())).HasValue());
        const auto capture = world->CaptureDebugSnapshot(CaptureRequest(*world, controller));
        REQUIRE(capture.snapshot->ProbeRetentionCapacity() == 0);
        REQUIRE(capture.snapshot->Probes().empty());
        REQUIRE(capture.snapshot->OmittedProbes() == 1);
        REQUIRE(capture.snapshot->ProbeAvailability() == CharacterDebugProbeAvailability::StorageUnavailable);
        REQUIRE(capture.snapshot->Capsule().radiusMeters == world->ControllerDescriptor(controller).Value().capsule.radiusMeters);
    }

    TEST_CASE("Character debug capture allocates nothing and retains the source Physics revision", "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        const auto request = CaptureRequest(*host.world, host.controller);
        const auto sourceRevision = host.world->Descriptor().physicsSnapshotRevision;
        REQUIRE(host.world->RefreshPhysicsSnapshot(request.physicsWorld, sourceRevision + 1).HasValue());
        const auto before = Tests::AllocationProbe::Count();
        const auto capture = host.world->CaptureDebugSnapshot(request);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(after == before);
        REQUIRE(capture.status == CharacterDebugCaptureStatus::Captured);
        REQUIRE(capture.snapshot->Identity().physicsSnapshotRevision == sourceRevision);
        REQUIRE(host.world->Descriptor().physicsSnapshotRevision == sourceRevision + 1);
    }

    TEST_CASE("Character debug custom-provider evidence reports unsupported probes and independent contact limits",
              "[character][debug-snapshot]") {
        auto host = SpawnedActiveWorldWithController();
        CharacterMovementResult evidence;
        evidence.contactCount = 1;
        evidence.collisions = CharacterCollisionFlags::Sides;
        evidence.contacts[0].shape = {host.world->Descriptor().physicsWorld, {8, 3}};
        evidence.contacts[0].material = Material();
        CharacterTickObserver observer;
        observer.context = &evidence;
        observer.movementResult = [](void *context, const CharacterMovementRequest &command,
                                     const CharacterTransformPublication &previous) noexcept {
            auto result = *static_cast<const CharacterMovementResult *>(context);
            result.controller = command.controller;
            result.tick = command.tick;
            result.sequence = command.sequence;
            result.finalPosition = previous.position;
            result.finalHeading = previous.heading;
            result.up = previous.up;
            return Result<CharacterMovementResult>::Success(result);
        };
        REQUIRE(host.world->QueueMovementCommand(Movement(host.controller, 1, 1)).HasValue());
        REQUIRE(host.world->AdvanceFixedTick(FixedTick(1, observer)).HasValue());
        auto request = CaptureRequest(*host.world, host.controller);
        request.maximumContacts = 0;
        const auto capture = host.world->CaptureDebugSnapshot(request);
        REQUIRE(capture.status == CharacterDebugCaptureStatus::CapacityLimited);
        REQUIRE(capture.snapshot->ProbeAvailability() == CharacterDebugProbeAvailability::UnsupportedProvider);
        REQUIRE(capture.snapshot->Probes().empty());
        REQUIRE(capture.snapshot->OmittedContacts() == 1);
        REQUIRE(capture.snapshot->Locomotion()->movement.contactCount == 0);
        REQUIRE(host.world->ControllerLocomotionSnapshot(host.controller).Value().movement.contactCount == 1);
    }
}  // namespace Horo::Character
