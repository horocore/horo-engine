#include "AllocationProbe.h"
#include "CharacterFastPathStorage.h"
#include "CharacterFoundationQualificationTestHelpers.h"
#include "CharacterReferenceSceneCorpusTestHelpers.h"
#include "Horo/Physics/CharacterCapability.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        struct EventRead final {
            std::array<CharacterMovementEvent, 32> events{};
            std::array<CharacterEventTerminalState, 4> terminal{};
            CharacterEventCapture receipt;
        };

        EventRead Read(const CharacterWorld &world, std::uint64_t after = 0) {
            EventRead read;
            read.receipt =
                world.CaptureEvents({world.Descriptor().sceneGeneration, world.Descriptor().identity, after}, read.events, read.terminal);
            return read;
        }

        bool Has(const EventRead &read, CharacterMovementEventKind kind) {
            return std::ranges::any_of(std::span{read.events}.first(read.receipt.copiedEvents), [kind](const auto &event) {
                return event.kind == kind;
            });
        }

        Physics::SurfaceMaterialId Surface(std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Physics::SurfaceMaterialId::FromBytes(bytes);
        }

        Physics::PhysicsSurfaceWorldBinding Surfaces() {
            auto catalog =
                Physics::SurfaceCatalogSnapshot::Create({"opaque-test-project", 3, {{Surface(1), "Stone"}, {Surface(2), "Wood"}}});
            REQUIRE(catalog.HasValue());
            return {std::make_shared<const Physics::SurfaceCatalogSnapshot>(std::move(catalog).Value()), 7, Surface(1)};
        }

        /** @brief Deterministic observer seam exercises publication/loss without creating another simulation owner. */
        struct Frame final {
            CharacterMovementResult movement;
            bool fail{};

            static Result<CharacterMovementResult> Resolve(void *context, const CharacterMovementRequest &request,
                                                           const CharacterTransformPublication &previous) noexcept {
                const auto &frame = *static_cast<Frame *>(context);
                if (frame.fail)
                    return Result<CharacterMovementResult>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
                auto result = frame.movement;
                result.controller = request.controller;
                result.tick = request.tick;
                result.sequence = request.sequence;
                result.finalPosition = previous.position;
                result.finalHeading = previous.heading;
                result.up = previous.up;
                return Result<CharacterMovementResult>::Success(result);
            }
        };

        void Advance(CharacterWorld &world, CharacterControllerHandle controller, std::uint64_t tick, Frame &frame) {
            REQUIRE(world.QueueMovementCommand(Movement(controller, tick, tick)).HasValue());
            auto input = FixedTick(tick, {.context = &frame, .movementResult = Frame::Resolve});
            const auto result = world.AdvanceFixedTick(input);
            if (result.HasError())
                INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
        }

        Frame WallFrame() {
            Frame frame;
            frame.movement.collisions = CharacterCollisionFlags::Sides;
            return frame;
        }
    }  // namespace

    TEST_CASE("Character committed events carry exact correlation and no-command ticks preserve source identity",
              "[physics][character][movement-events]") {
        auto active = QualificationTest::QualificationSpawnedWorld();
        auto wall = WallFrame();
        Advance(*active.world, active.controller, 1, wall);
        const auto first = Read(*active.world);
        REQUIRE(first.receipt.status == CharacterEventCaptureStatus::Captured);
        REQUIRE(first.receipt.copiedEvents == 1);
        const auto &event = first.events[0];
        const auto source = active.world->ControllerLocomotionSnapshot(active.controller).Value();
        CHECK(event.kind == CharacterMovementEventKind::HitWall);
        CHECK(event.controller == active.controller);
        CHECK(event.tick == 1);
        CHECK(event.sequence == 1);
        CHECK(event.stateRevision == source.stateRevision);
        CHECK(event.publicationRevision == source.transform.publicationRevision);
        CHECK(event.streamPublicationRevision == first.receipt.publicationRevision);
        Advance(*active.world, active.controller, 2, wall);
        const auto repeated = Read(*active.world, first.receipt.publicationRevision);
        CHECK(repeated.receipt.emittedEvents == 0);
        CHECK(repeated.receipt.copiedEvents == 0);
        REQUIRE(active.world->AdvanceFixedTick(FixedTick(3)).HasValue());
        const auto empty = Read(*active.world, repeated.receipt.publicationRevision);
        CHECK(empty.receipt.tick == 3);
        CHECK(empty.receipt.copiedEvents == 0);
        REQUIRE(empty.receipt.copiedTerminalStates == 1);
        CHECK(empty.terminal[0].sourceTick == 2);
        CHECK(empty.terminal[0].sequence == 2);
        CHECK(Read(*active.world, empty.receipt.publicationRevision).receipt.status == CharacterEventCaptureStatus::NoNewPublication);
    }

    TEST_CASE("Character event loss cannot discard terminal state or mutate canonical movement",
              "[physics][character][movement-events][capacity]") {
        CharacterWorldSettingsDescriptor settings;
        settings.capacities.maximumControllers = 1;
        settings.capacities.maximumQueuedEvents = 1;
        auto prepared = CharacterWorld::Prepare(WorldDescriptor(), CharacterWorldSettings::Capture(settings).Value());
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        auto descriptor = ControllerDescriptor(world->Descriptor());
        descriptor.steepSlopePolicy = CharacterSteepSlopePolicy::Slide;
        const auto controller = world->CreateController(descriptor).Value();
        REQUIRE(world->Activate().HasValue());
        QualificationTest::ClearPlacement placement;
        REQUIRE(world->SpawnController(controller, placement.Context(world->Descriptor())).HasValue());
        auto frame = WallFrame();
        frame.movement.collisions = CharacterCollisionFlags::Sides | CharacterCollisionFlags::Ceiling;
        frame.movement.sliding = true;
        Advance(*world, controller, 1, frame);
        const auto read = Read(*world);
        CHECK(read.receipt.emittedEvents == 3);
        CHECK(read.receipt.droppedEvents == 2);
        CHECK(read.receipt.cumulativeDroppedEvents == 2);
        CHECK(read.receipt.copiedEvents == 1);
        REQUIRE(read.receipt.copiedTerminalStates == 1);
        CHECK(read.terminal[0].collisions == frame.movement.collisions);
        CHECK(read.terminal[0].sliding);
        CHECK_FALSE(Has(read, CharacterMovementEventKind::SlideStart));
        CHECK(world->ControllerLocomotionSnapshot(controller).Value().movement.collisions == frame.movement.collisions);
        std::array<CharacterMovementEvent, 1> sentinel;
        sentinel[0].tick = 99;
        const auto shortTerminal = world->CaptureEvents({61, world->Descriptor().identity, 0}, sentinel, {});
        CHECK(shortTerminal.status == CharacterEventCaptureStatus::CapacityLimited);
        CHECK(sentinel[0].tick == 99);
        std::array<CharacterEventTerminalState, 1> terminal;
        const auto shortEvents = world->CaptureEvents({61, world->Descriptor().identity, 0}, {}, terminal);
        CHECK(shortEvents.status == CharacterEventCaptureStatus::CapacityLimited);
        CHECK(shortEvents.readDroppedEvents == 1);
        CHECK(shortEvents.copiedTerminalStates == 1);
        frame.movement.sliding = false;
        Advance(*world, controller, 2, frame);
        const auto ended = Read(*world, read.receipt.publicationRevision);
        CHECK(ended.receipt.emittedEvents == 1);
        CHECK(ended.receipt.droppedEvents == 1);  // Old retained HitWall is evicted even though this consumer already read it.
        CHECK(ended.receipt.cumulativeDroppedEvents == 3);
        REQUIRE(ended.receipt.copiedEvents == 1);
        CHECK(ended.events[0].kind == CharacterMovementEventKind::SlideEnd);
        CHECK_FALSE(ended.terminal[0].sliding);
        CHECK_FALSE(world->ControllerLocomotionSnapshot(controller).Value().movement.sliding);
    }

    TEST_CASE("Character failed ticks and retired consumers cannot publish or replay new facts",
              "[physics][character][movement-events][lifecycle]") {
        auto active = QualificationTest::QualificationSpawnedWorld();
        auto client = active.world->IssueCapability().Value();
        auto wall = WallFrame();
        Advance(*active.world, active.controller, 1, wall);
        const auto retained = Read(*active.world);
        wall.fail = true;
        REQUIRE(active.world->QueueMovementCommand(Movement(active.controller, 2, 2)).HasValue());
        CHECK(active.world->AdvanceFixedTick(FixedTick(2, {.context = &wall, .movementResult = Frame::Resolve})).HasError());
        CHECK(Read(*active.world).receipt.publicationRevision == retained.receipt.publicationRevision);
        CHECK(Read(*active.world).events[0].tick == retained.events[0].tick);
        active.world->Shutdown();
        std::array<CharacterMovementEvent, 4> events;
        std::array<CharacterEventTerminalState, 1> states;
        CHECK(client.CaptureEvents({61, active.world->Descriptor().identity, 0}, events, states).HasError());
        CHECK(Read(*active.world).receipt.status == CharacterEventCaptureStatus::Closed);
        CHECK(retained.events[0].kind == CharacterMovementEventKind::HitWall);
    }

    TEST_CASE("Character event projection capture and no-consumer ticks allocate no hot-path storage",
              "[physics][character][movement-events][allocation]") {
        auto active = QualificationTest::QualificationSpawnedWorld();
        auto wall = WallFrame();
        auto command = Movement(active.controller, 1, 1);
        auto input = FixedTick(1, {.context = &wall, .movementResult = Frame::Resolve});
        std::array<CharacterMovementEvent, 32> events;
        std::array<CharacterEventTerminalState, 1> terminal;
        const auto before = Tests::AllocationProbe::Count();
        const auto admitted = active.world->QueueMovementCommand(command);
        const auto advanced = active.world->AdvanceFixedTick(input);
        const auto capture = active.world->CaptureEvents({61, active.world->Descriptor().identity, 0}, events, terminal);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(admitted.HasValue());
        REQUIRE(advanced.HasValue());
        CHECK(capture.copiedEvents == 1);
        CHECK(after == before);
    }

    TEST_CASE("Character semantic defaults are explicit catalogue members and legacy absence remains unavailable",
              "[physics][character][movement-events][surface]") {
        auto binding = Surfaces();
        auto prepared = CharacterWorld::Prepare(WorldDescriptor(), Settings(1), binding);
        REQUIRE(prepared.HasValue());
        auto &world = *prepared.Value();
        auto descriptor = ControllerDescriptor(world.Descriptor());
        descriptor.defaultSurface = Surface(99);
        RequireError(world.CreateController(descriptor), CharacterErrors::DescriptorInvalid);
        CHECK(world.ActiveControllerCount() == 0);
        descriptor.defaultSurface = Surface(2);
        const auto controller = world.CreateController(descriptor);
        REQUIRE(controller.HasValue());
        CHECK(world.ControllerDescriptor(controller.Value()).Value().defaultSurface == Surface(2));
        binding.defaultSurface.reset();
        auto unconfigured = CharacterWorld::Prepare(WorldDescriptor(), Settings(1), binding);
        REQUIRE(unconfigured.HasValue());
        RequireError(unconfigured.Value()->CreateController(ControllerDescriptor(unconfigured.Value()->Descriptor())),
                     CharacterErrors::CapabilityUnavailable);
        auto legacy = QualificationTest::QualificationSpawnedWorld();
        auto frame = WallFrame();
        Advance(*legacy.world, legacy.controller, 1, frame);
        CHECK(Read(*legacy.world).terminal[0].surfaceAvailability == CharacterSurfaceAvailability::Unavailable);
    }

    TEST_CASE("Character event captures respect delegated revocation generation and owner-thread lifetime",
              "[physics][character][movement-events][lifecycle]") {
        auto active = QualificationTest::QualificationSpawnedWorld();
        auto parent = active.world->IssueCapability().Value();
        auto child = parent.Derive().Value();
        auto frame = WallFrame();
        Advance(*active.world, active.controller, 1, frame);
        EventRead read;
        const CharacterEventCaptureRequest request{61, active.world->Descriptor().identity, 0};
        REQUIRE(child.CaptureEvents(request, read.events, read.terminal).HasValue());
        parent.Revoke();
        RequireError(child.CaptureEvents(request, read.events, read.terminal), CharacterErrors::CapabilityRevoked);
        auto wrongGeneration = request;
        ++wrongGeneration.sceneGeneration;
        CHECK(active.world->CaptureEvents(wrongGeneration, read.events, read.terminal).status == CharacterEventCaptureStatus::Stale);
        CharacterEventCapture threaded;
        std::thread worker([&] {
            threaded = active.world->CaptureEvents(request, read.events, read.terminal);
        });
        worker.join();
        CHECK(threaded.status == CharacterEventCaptureStatus::WrongThread);
        auto client = active.world->IssueCapability().Value();
        active.world.reset();
        RequireError(client.CaptureEvents(request, read.events, read.terminal), CharacterErrors::CapabilityStale);
    }

    TEST_CASE("Character event ordering is independent of producer admission order", "[physics][character][movement-events][ordering]") {
        for (const bool reverse : {false, true}) {
            auto active = ActiveWorldWithControllers(2);
            QualificationTest::ClearPlacement placement;
            for (const auto handle : active.controllers)
                REQUIRE(active.world->SpawnController(handle, placement.Context(active.world->Descriptor())).HasValue());
            for (std::size_t index{}; index < active.controllers.size(); ++index) {
                const auto handle = active.controllers[reverse ? 1 - index : index];
                REQUIRE(active.world->QueueMovementCommand(Movement(handle, 1, 1)).HasValue());
            }
            auto wall = WallFrame();
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, {.context = &wall, .movementResult = Frame::Resolve})).HasValue());
            const auto read = Read(*active.world);
            REQUIRE(read.receipt.copiedEvents == 2);
            CHECK(read.events[0].controller == active.controllers[0]);
            CHECK(read.events[1].controller == active.controllers[1]);
            CHECK(read.receipt.copiedTerminalStates == 2);
        }
    }

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Both native supports derive physical evidence from the sole canonical material codec. */
        Physics::PhysicsQueryMaterial CanonicalPhysicalMaterial() {
            const auto material = Physics::PhysicsMaterialAsset::Create(
                {.id = Physics::PhysicsMaterialAssetId::FromAssetId(Material().asset), .sourceRevision = 3});
            REQUIRE(material.HasValue());
            const auto encoded = Physics::EncodePhysicsMaterialAsset(material.Value());
            REQUIRE(encoded.HasValue());
            const auto decoded = Physics::DecodePhysicsMaterialAsset(encoded.Value());
            REQUIRE(decoded.HasValue());
            return {decoded.Value().Id().Asset(), decoded.Value().SourceRevision(), Material().slot};
        }

        struct SemanticNativeWorld final {
            Physics::PhysicsSurfaceWorldBinding binding{Surfaces()};
            Physics::PhysicsQueryMaterial physical{CanonicalPhysicalMaterial()};
            std::unique_ptr<Physics::PhysicsRuntime> runtime{
                Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value()};
            std::unique_ptr<Physics::PhysicsWorld> physics;
            std::unique_ptr<CharacterWorld> character;
            CharacterControllerHandle controller;

            SemanticNativeWorld() {
                physics = runtime->PrepareWorld(Physics::Test::SmallWorldSettings(), {.surfaces = binding}).Value();
                REQUIRE(physics->Activate(runtime->IssueWorldIdentity().Value()).HasValue());
                character = CharacterWorld::Prepare({61, physics->Identity(), 1, 1}, Settings(1), binding).Value();
                AddSupport(-1.F, Surface(1));
                AddSupport(1.F, Surface(2));
                REQUIRE(physics
                            ->AdvanceFixedTick(
                                {.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
                REQUIRE(character->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasValue());
                auto descriptor = ControllerDescriptor(character->Descriptor());
                descriptor.defaultMaterial = physical;
                descriptor.capsule = {0.25F, 0.5F};
                descriptor.collisionRootPosition = {-1.5F, 0.77F, 0};
                descriptor.gravity = {};
                controller = character->CreateController(descriptor).Value();
                REQUIRE(character->Activate().HasValue());
                CharacterPhysicsQueryAdapter adapter(*physics);
                REQUIRE(
                    character->SpawnController(controller, adapter.Context(ReferenceTest::QueryExpectations(character->Descriptor(), 0)))
                        .HasValue());
            }

            void AddSupport(float x, Physics::SurfaceMaterialId surface) {
                const auto policy = ControllerDescriptor(character->Descriptor());
                REQUIRE(physics
                            ->CreateQueryFixture({.shape = Physics::PhysicsBoxShape{{1, 0.5F, 2}},
                                                  .pose = {.translation = {x, -0.5F, 0}},
                                                  .layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                                                  .profile = policy.collisionProfile,
                                                  .channel = policy.queryChannel,
                                                  .material = physical,
                                                  .surface = Physics::MakePhysicsSurfaceEvidence(binding, surface)})
                            .HasValue());
            }

            void Move(std::uint64_t tick) {
                auto command = Movement(controller, tick, tick);
                command.desiredVelocityMetersPerSecond = Math::Vec3{10, 0, 0};
                REQUIRE(character->QueueMovementCommand(command).HasValue());
                auto input = FixedTick(tick);
                input.fixedDelta = Duration::FromNanoseconds(100'000'000);
                CharacterPhysicsQueryAdapter adapter(*physics);
                input.query = adapter.Context(ReferenceTest::QueryExpectations(character->Descriptor(), tick));
                const auto moved = character->AdvanceFixedTick(input);
                if (moved.HasError())
                    INFO(moved.ErrorValue().message);
                REQUIRE(moved.HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Native Character publishes SurfaceChanged from admitted semantics despite identical physical materials",
              "[physics][character][movement-events][native]") {
        SemanticNativeWorld host;
        host.Move(1);
        const auto first = Read(*host.character);
        REQUIRE(Has(first, CharacterMovementEventKind::Landed));
        const auto initial = host.character->ControllerLocomotionSnapshot(host.controller).Value();
        REQUIRE(initial.movement.groundSurface);
        CHECK(initial.movement.groundSurface->surface == Surface(1));
        host.Move(2);
        const auto changed = Read(*host.character, first.receipt.publicationRevision);
        REQUIRE(changed.receipt.copiedEvents == 1);
        CHECK(changed.events[0].kind == CharacterMovementEventKind::SurfaceChanged);
        REQUIRE(changed.events[0].contact);
        REQUIRE(changed.events[0].contact->surface);
        CHECK(changed.events[0].contact->surface->surface == Surface(2));
        CHECK(changed.events[0].contact->surfaceSource == CharacterSemanticSurfaceSource::Query);
        const auto finalMaterial = host.character->ControllerLocomotionSnapshot(host.controller).Value().movement.groundMaterial;
        REQUIRE(finalMaterial);
        REQUIRE(initial.movement.groundMaterial);
        CHECK(finalMaterial->asset == initial.movement.groundMaterial->asset);
        CHECK(finalMaterial->slot == initial.movement.groundMaterial->slot);
        CHECK(finalMaterial->assetGeneration == initial.movement.groundMaterial->assetGeneration);
        CHECK(changed.terminal[0].groundSurface->surface == Surface(2));
    }

    TEST_CASE("Native Character wall and ceiling contact entry facts do not repeat on sustained contact",
              "[physics][character][movement-events][native]") {
        const bool ceiling = GENERATE(false, true);
        ReferenceTest::ReferenceWorld host;
        ReferenceTest::ReferencePlan plan;
        ReferenceTest::ConfigureBarriers(host, ceiling ? Corpus::ReferenceScene::Ceiling : Corpus::ReferenceScene::ThinWall, false, plan);
        host.Spawn(plan.start, plan.gravity);
        CharacterMetricCapture capture;
        REQUIRE(host.Move(plan.velocity, capture).HasValue());
        const auto first = Read(*host.character);
        const auto kind = ceiling ? CharacterMovementEventKind::HitCeiling : CharacterMovementEventKind::HitWall;
        REQUIRE(Has(first, kind));
        REQUIRE(host.Move(plan.velocity, capture, 2).HasValue());
        CHECK_FALSE(Has(Read(*host.character, first.receipt.publicationRevision), kind));
    }

    TEST_CASE("Native Character commits Landed and LeftGround once across airborne intent",
              "[physics][character][movement-events][native]") {
        ReferenceTest::ReferenceWorld host;
        host.Add(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0});
        host.Spawn();
        CharacterMetricCapture capture;
        REQUIRE(host.Move({}, capture).HasValue());
        const auto grounded = Read(*host.character);
        REQUIRE(Has(grounded, CharacterMovementEventKind::Landed));
        REQUIRE(host.Move({0, 10, 0}, capture, 2).HasValue());
        const auto airborne = Read(*host.character, grounded.receipt.publicationRevision);
        CHECK(Has(airborne, CharacterMovementEventKind::LeftGround));
        CHECK_FALSE(airborne.terminal[0].grounded);
        REQUIRE(host.Move({0, 10, 0}, capture, 3).HasValue());
        CHECK_FALSE(Has(Read(*host.character, airborne.receipt.publicationRevision), CharacterMovementEventKind::LeftGround));
    }

    TEST_CASE("Native Character commits actual steep sliding and publishes its terminal end transition",
              "[physics][character][movement-events][native]") {
        ReferenceTest::ReferenceWorld host;
        const Math::Vec3 normal{-std::sqrt(0.75F), 0.5F, 0};
        host.Add(Physics::PhysicsStaticPlaneShape{normal, 0});
        REQUIRE(host.physics
                    ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 61, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(
            host.character->RefreshPhysicsSnapshot(host.physics->Identity(), host.physics->PublishedTick().publicationRevision).HasValue());
        auto descriptor = ControllerDescriptor(host.character->Descriptor());
        descriptor.capsule = {0.25F, 0.5F};
        descriptor.collisionRootPosition = {0, 1.02F, 0};
        descriptor.maximumStepHeightMeters = 0;
        descriptor.gravity = {0, -9.81F, 0};
        descriptor.steepSlopePolicy = CharacterSteepSlopePolicy::Slide;
        const auto created = host.character->CreateController(descriptor);
        REQUIRE(created.HasValue());
        host.controller = created.Value();
        REQUIRE(host.character->Activate().HasValue());
        CharacterPhysicsQueryAdapter adapter(*host.physics);
        REQUIRE(host.character
                    ->SpawnController(host.controller, adapter.Context(ReferenceTest::QueryExpectations(host.character->Descriptor(), 0)))
                    .HasValue());
        CharacterMetricCapture capture;
        REQUIRE(host.Move({}, capture).HasValue());
        const auto sliding = Read(*host.character);
        REQUIRE(sliding.terminal[0].sliding);
        REQUIRE(Has(sliding, CharacterMovementEventKind::SlideStart));
        REQUIRE(host.Move({}, capture, 2).HasValue());
        const auto sustained = Read(*host.character, sliding.receipt.publicationRevision);
        CHECK(sustained.terminal[0].sliding);
        CHECK_FALSE(Has(sustained, CharacterMovementEventKind::SlideStart));
        REQUIRE(host.Move({0, 10, 0}, capture, 3).HasValue());
        const auto ended = Read(*host.character, sustained.receipt.publicationRevision);
        CHECK_FALSE(ended.terminal[0].sliding);
        CHECK(Has(ended, CharacterMovementEventKind::SlideEnd));
    }
#endif
}  // namespace Horo::Character
