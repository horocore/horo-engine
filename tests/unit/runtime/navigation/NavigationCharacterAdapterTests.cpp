#include "../physics/CharacterWorldTestHelpers.h"
#include "Horo/Navigation/NavigationCharacterAdapter.h"
#include "navigation/NavigationVelocityTestFixture.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Navigation {
    namespace {
        using namespace VelocityTestSupport;
        namespace CharacterTest = Character::TestDetail;

        struct BridgeFixture final {
            VelocityFixture nav{{.blockedTicksBeforeRepath = 2, .repathCooldownTicks = 10}};
            std::unique_ptr<Character::CharacterWorld> world;
            Character::CharacterControllerHandle controller;
            Character::CharacterControllerHandle independentController;
            Character::CharacterCapability grant;
            NavigationCharacterAdapter adapter;

            BridgeFixture() {
                auto descriptor = CharacterTest::WorldDescriptor();
                descriptor.sceneGeneration = nav.binding.sceneGeneration.Value();
                Character::CharacterWorldSettingsDescriptor settings;
                settings.capacities.maximumControllers = 2;
                settings.capacities.maximumQueuedCommands = 1;
                settings.work.maximumCommandsPerTick = 1;
                world =
                    std::move(Character::CharacterWorld::Prepare(descriptor, Character::CharacterWorldSettings::Capture(settings).Value()))
                        .Value();
                controller = world->CreateController(CharacterTest::ControllerDescriptor(world->Descriptor())).Value();
                independentController = world->CreateController(CharacterTest::ControllerDescriptor(world->Descriptor())).Value();
                REQUIRE(world->Activate().HasValue());
                CharacterTest::OverlapProbe probe;
                REQUIRE(world->SpawnController(controller, probe.Context(world->Descriptor())).HasValue());
                REQUIRE(world->SpawnController(independentController, probe.Context(world->Descriptor())).HasValue());
                grant = world->IssueCapability().Value();
                adapter = std::move(NavigationCharacterAdapter::Create(*nav.pipeline, grant, Binding())).Value();
                nav.PublishRoute();
            }

            NavigationCharacterBinding Binding() const {
                return {{1, 0, CharacterInput::DesiredMotionSource::ExternalIntent, true}, controller, nav.binding, 1};
            }

            NavigationVelocityPublication Publish(const std::uint64_t tick) {
                nav.position = world->ControllerTransform(controller).Value().position;
                return nav.Publish(nav.Capture(tick));
            }

            NavigationCharacterMotion Capture(const NavigationVelocityPublication &publication) {
                auto captured = adapter.Prepare(publication, nav.Now(publication.intent.tick), publication.intent.tick + 100);
                REQUIRE(captured.HasValue());
                return captured.Value();
            }

            void Advance(const std::uint64_t tick, const Character::CharacterTickObserver observer = {}) {
                auto input = CharacterTest::FixedTick(tick, observer);
                input.sceneGeneration = nav.binding.sceneGeneration.Value();
                input.fixedDelta = Duration::FromNanoseconds(100'000'000);
                REQUIRE(world->AdvanceFixedTick(input).HasValue());
            }
        };

        struct BlockedCharacter final {
            static Result<Character::CharacterMovementResult> Resolve(void *, const Character::CharacterMovementRequest &command,
                                                                      const Character::CharacterTransformPublication &previous) noexcept {
                Character::CharacterMovementResult result;
                result.controller = command.controller;
                result.tick = command.tick;
                result.sequence = command.sequence;
                result.finalPosition = previous.position;
                result.finalHeading = previous.heading;
                result.up = previous.up;
                result.collisions = Character::CharacterCollisionFlags::Sides;
                return Result<Character::CharacterMovementResult>::Success(result);
            }
        };
    }  // namespace

    TEST_CASE("Navigation Character bridge preserves canonical command correlation and actual achieved root feedback",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto before = fixture.world->ControllerTransform(fixture.controller).Value();
        const auto publication = fixture.Publish(1);
        const auto work = fixture.Capture(publication);
        REQUIRE(work.Tick() == 1);
        REQUIRE(work.Correlation() == 101);
        const auto admitted = fixture.adapter.Submit(work, fixture.nav.Now(1));
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().admission.status == Character::CharacterCommandAdmissionStatus::Deferred);
        CHECK(fixture.world->ControllerTransform(fixture.controller).Value().publicationRevision == before.publicationRevision);
        CHECK(fixture.adapter.ObserveFeedback().HasError());
        fixture.Advance(1);
        const auto feedback = fixture.adapter.ObserveFeedback();
        REQUIRE(feedback.HasValue());
        CHECK(feedback.Value().movement.sequence == work.Correlation());
        CHECK(feedback.Value().movement.achievedVelocityMetersPerSecond == publication.intent.desiredVelocity);
        CHECK(feedback.Value().transform.position.x == Catch::Approx(publication.intent.desiredVelocity.x * 0.1F));
        CHECK(feedback.Value().transform.publicationRevision > before.publicationRevision);
        CHECK(fixture.adapter.ObserveFeedback().HasError());
        CHECK(fixture.Publish(2).intent.state == NavigationMovementState::Following);
    }

    TEST_CASE("Navigation Character feedback preserves controller resolved motion instead of echoing desired translation",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto publication = fixture.Publish(1);
        REQUIRE(publication.intent.desiredVelocity.x > 0);
        REQUIRE(fixture.adapter.Submit(fixture.Capture(publication), fixture.nav.Now(1)).HasValue());
        // A host Character resolver may choose different root motion or a step result. The bridge is only a reader.
        auto physicsWorld = fixture.world->Descriptor().physicsWorld;
        const auto resolved = [](void *context, const Character::CharacterMovementRequest &command,
                                 const Character::CharacterTransformPublication &previous) noexcept {
            Character::CharacterMovementResult result;
            result.controller = command.controller;
            result.tick = command.tick;
            result.sequence = command.sequence;
            result.finalPosition = previous.position + Math::Vec3{0, 0, 0.05F};
            result.finalHeading = previous.heading;
            result.achievedVelocityMetersPerSecond = {0, 0, 0.5F};
            result.up = previous.up;
            result.grounded = true;
            result.groundMaterial = CharacterTest::Material();
            result.groundShape = Physics::ShapeHandle{*static_cast<Physics::PhysicsWorldId *>(context), {8, 3}};
            result.groundDistanceMeters = 0.02F;
            result.collisions = Character::CharacterCollisionFlags::Ground | Character::CharacterCollisionFlags::Step;
            return Result<Character::CharacterMovementResult>::Success(result);
        };
        fixture.Advance(1, {.context = &physicsWorld, .movementResult = resolved});
        const auto feedback = fixture.adapter.ObserveFeedback();
        REQUIRE(feedback.HasValue());
        CHECK(feedback.Value().movement.achievedVelocityMetersPerSecond == Math::Vec3{0, 0, 0.5F});
        CHECK(feedback.Value().movement.collisions ==
              (Character::CharacterCollisionFlags::Ground | Character::CharacterCollisionFlags::Step));
        CHECK(feedback.Value().movement.grounded);
        CHECK(feedback.Value().transform.grounded);
        REQUIRE(feedback.Value().movement.groundMaterial.has_value());
        CHECK(feedback.Value().movement.groundMaterial->asset == CharacterTest::Material().asset);
        CHECK(feedback.Value().movement.groundMaterial->assetGeneration == CharacterTest::Material().assetGeneration);
        CHECK(feedback.Value().movement.groundMaterial->slot == CharacterTest::Material().slot);
        CHECK(feedback.Value().transform.position.z == Catch::Approx(0.05F));
        CHECK(fixture.Publish(2).intent.state == NavigationMovementState::Following);
    }

    TEST_CASE("Navigation Character bridge rejects forged publications and stale owner authority before admission",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto publication = fixture.Publish(1);
        auto forged = publication;
        forged.intent.desiredVelocity = {50, 0, 0};
        CHECK(fixture.adapter.Prepare(forged, fixture.nav.Now(1), 101).HasError());
        forged = publication;
        ++forged.characterRevision;
        CHECK(fixture.adapter.Prepare(forged, fixture.nav.Now(1), 101).HasError());
        auto wrongStep = fixture.nav.Now(1);
        wrongStep.stepSeconds *= 2;
        CHECK(fixture.adapter.Prepare(publication, wrongStep, 101).HasError());
        const auto work = fixture.Capture(publication);
        SECTION("cancelled goal") {
            REQUIRE(fixture.nav.movement->Cancel(publication.intent.operation));
        }
        SECTION("retired pipeline") {
            fixture.nav.pipeline->Retire();
        }
        SECTION("revoked grant") {
            fixture.grant.Revoke();
        }
        SECTION("deleted agent") {
            REQUIRE(fixture.nav.registry.UnregisterAtSafePoint(fixture.nav.agent).HasValue());
        }
        CHECK(fixture.adapter.Submit(work, fixture.nav.Now(1)).HasError());
        CHECK(fixture.world->TickStatistics().pendingCommands == 0);
    }

    TEST_CASE("Navigation Character bridge retains immutable work for full queue retry without duplicating admission",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        static_cast<void>(fixture.Publish(1));
        const auto publication = fixture.Publish(2);
        const auto work = fixture.Capture(publication);
        const auto filler = fixture.world->IssueCapability().Value();
        REQUIRE(filler.QueueMovementCommand(CharacterTest::Movement(fixture.independentController, 1, 1)).Value().status ==
                Character::CharacterCommandAdmissionStatus::Deferred);
        const auto full = fixture.adapter.Submit(work, fixture.nav.Now(2));
        REQUIRE(full.HasValue());
        CHECK(full.Value().admission.status == Character::CharacterCommandAdmissionStatus::RejectedFull);
        CHECK(fixture.adapter.ObserveFeedback().HasError());
        fixture.Advance(1);  // Only the independent controller changes; captured tick-two root evidence stays current.
        REQUIRE(fixture.adapter.Submit(work, fixture.nav.Now(2)).Value().admission.status ==
                Character::CharacterCommandAdmissionStatus::Deferred);
        CHECK(fixture.adapter.Submit(work, fixture.nav.Now(2)).HasError());
        fixture.Advance(2);
        REQUIRE(fixture.adapter.ObserveFeedback().HasValue());
    }

    TEST_CASE("Navigation Character bridge delegates blockage hysteresis to the existing bounded repath policy",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        for (std::uint64_t tick = 1; tick <= 2; ++tick) {
            const auto publication = fixture.Publish(tick);
            REQUIRE(publication.intent.state == NavigationMovementState::Following);
            REQUIRE(fixture.adapter.Submit(fixture.Capture(publication), fixture.nav.Now(tick)).HasValue());
            fixture.Advance(tick, {.movementResult = BlockedCharacter::Resolve});
            const auto feedback = fixture.adapter.ObserveFeedback();
            REQUIRE(feedback.HasValue());
            CHECK(feedback.Value().movement.achievedVelocityMetersPerSecond == Math::Vec3{});
            CHECK(feedback.Value().movement.collisions == Character::CharacterCollisionFlags::Sides);
        }
        CHECK(fixture.Publish(3).intent.state == NavigationMovementState::Querying);
        CHECK(fixture.Publish(4).intent.state == NavigationMovementState::Querying);
    }

    TEST_CASE("Navigation Character bridge revokes pending commands on shutdown and moves grant ownership exactly once",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto publication = fixture.Publish(1);
        const auto work = fixture.Capture(publication);
        auto moved = std::move(fixture.adapter);
        CHECK(fixture.adapter.Submit(work, fixture.nav.Now(1)).HasError());
        REQUIRE(moved.Submit(work, fixture.nav.Now(1)).HasValue());
        moved.Shutdown();
        moved.Shutdown();
        CHECK(fixture.grant.ControllerTransform(fixture.controller).HasError());
        fixture.Advance(1);
        CHECK(fixture.world->ControllerLocomotionSnapshot(fixture.controller).HasError());
    }

    TEST_CASE("Navigation Character bridge rejects teleport discontinuities before submission and historical feedback",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto publication = fixture.Publish(1);
        const auto work = fixture.Capture(publication);
        CharacterTest::OverlapProbe probe;
        SECTION("teleport before queue admission") {
            REQUIRE(fixture.world->TeleportController({fixture.controller, 1, {3, 0, 0}}, probe.Context(fixture.world->Descriptor(), 1))
                        .HasValue());
            CHECK(fixture.adapter.Submit(work, fixture.nav.Now(1)).HasError());
        }
        SECTION("teleport after completed movement before feedback") {
            REQUIRE(fixture.adapter.Submit(work, fixture.nav.Now(1)).HasValue());
            fixture.Advance(1);
            REQUIRE(fixture.world->TeleportController({fixture.controller, 2, {3, 0, 0}}, probe.Context(fixture.world->Descriptor(), 2))
                        .HasValue());
            CHECK(fixture.adapter.ObserveFeedback().HasError());
        }
        fixture.adapter.DiscardFeedback();
        fixture.adapter.Shutdown();
        CHECK(fixture.adapter.ObserveFeedback().HasError());
    }

    TEST_CASE("Navigation Character bridge submits an explicit collision safe zero rather than absent motion",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        const auto snapshot = fixture.nav.Capture(1);
        const auto pending = fixture.nav.Prepare(snapshot);
        auto outcome = fixture.nav.solver.Solve(snapshot, pending.request).Value();
        outcome.desiredVelocity = {};
        outcome.disposition = NavigationAvoidanceDisposition::CollisionSafeStop;
        outcome.stopReason = NavigationAvoidanceStopReason::NoFeasibleSample;
        const auto publication = fixture.nav.pipeline->Commit(pending, outcome, snapshot, fixture.nav.Now(1));
        REQUIRE(publication.HasValue());
        const auto admitted = fixture.adapter.Submit(fixture.Capture(publication.Value()), fixture.nav.Now(1));
        REQUIRE(admitted.HasValue());
        CHECK_FALSE(admitted.Value().absent);
        CharacterTest::CommandTrace trace;
        fixture.Advance(1, trace.Observer());
        REQUIRE(trace.movementCount == 1);
        CHECK(trace.movements[0].desiredVelocityMetersPerSecond == Math::Vec3{});
        REQUIRE(fixture.adapter.ObserveFeedback().HasValue());
    }

    TEST_CASE("Navigation Character bridge default authority fails closed without revoking a rejected grant",
              "[navigation][character][bridge]") {
        BridgeFixture fixture;
        auto denied = fixture.Binding();
        denied.principal.permissionGranted = false;
        const auto independent = fixture.world->IssueCapability().Value();
        CHECK(NavigationCharacterAdapter::Create(*fixture.nav.pipeline, independent, denied).HasError());
        CHECK(independent.ControllerTransform(fixture.controller).HasValue());
        denied = fixture.Binding();
        denied.characterRevision = 0;
        CHECK(NavigationCharacterAdapter::Create(*fixture.nav.pipeline, independent, denied).HasError());
        denied = fixture.Binding();
        denied.principal.source = CharacterInput::DesiredMotionSource::GameplayInput;
        CHECK(NavigationCharacterAdapter::Create(*fixture.nav.pipeline, independent, denied).HasError());
    }
}  // namespace Horo::Navigation
