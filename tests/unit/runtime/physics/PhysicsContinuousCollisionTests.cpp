#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <iostream>
#include <limits>

namespace Horo::Physics {
    TEST_CASE("CCD policy fractions and unknown modes reject before world capture", "[physics][ccd][settings]") {
        for (const float value : {0.0F, -1.0F, 1.01F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            PhysicsContinuousCollisionPolicy policy;
            policy.thresholdFraction = value;
            Test::RequireError(ValidatePhysicsContinuousCollisionPolicy(policy), PhysicsErrors::DescriptorInvalid);
            policy = {};
            policy.penetrationFraction = value;
            Test::RequireError(ValidatePhysicsContinuousCollisionPolicy(policy), PhysicsErrors::DescriptorInvalid);
        }
        PhysicsContinuousCollisionPolicy unknown;
        unknown.defaultMode = static_cast<PhysicsDefaultMotionQuality>(255);
        Test::RequireError(ValidatePhysicsContinuousCollisionPolicy(unknown), PhysicsErrors::OperationUnsupported);
        PhysicsWorldSettingsDescriptor descriptor;
        const auto discrete = PhysicsWorldSettings::Capture(descriptor);
        REQUIRE(discrete.HasValue());
        descriptor.step.defaultMotionQuality = PhysicsDefaultMotionQuality::LinearCast;
        const auto ccd = PhysicsWorldSettings::Capture(descriptor);
        REQUIRE(ccd.HasValue());
        REQUIRE(ccd.Value().Identity() != discrete.Value().Identity());
        descriptor.step.linearCastThresholdFraction = 0.5F;
        const auto threshold = PhysicsWorldSettings::Capture(descriptor);
        REQUIRE(threshold.HasValue());
        REQUIRE(threshold.Value().Identity() != ccd.Value().Identity());
    }

    TEST_CASE("Explicit body CCD does not turn static or kinematic motion into a dynamic fallback", "[physics][ccd][descriptor]") {
        PhysicsAuthoredBodyDescriptor body;
        body.continuousCollision.mode = PhysicsDefaultMotionQuality::LinearCast;
        Test::RequireError(ValidatePhysicsAuthoredBodyDescriptor(body), PhysicsErrors::OperationUnsupported);
        body.motion = PhysicsMotionType::Kinematic;
        Test::RequireError(ValidatePhysicsAuthoredBodyDescriptor(body), PhysicsErrors::OperationUnsupported);
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{};
        REQUIRE(ValidatePhysicsAuthoredBodyDescriptor(body).HasValue());
        body.continuousCollision.mode = static_cast<PhysicsDefaultMotionQuality>(255);
        Test::RequireError(ValidatePhysicsAuthoredBodyDescriptor(body), PhysicsErrors::OperationUnsupported);
    }

    TEST_CASE("Null composition cannot issue CCD policy evidence or admit a native CCD command", "[physics][ccd][omitted]") {
        auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        Test::RequireError(runtime->PrepareWorld(Test::SmallWorldSettings(), {{}, 1}), PhysicsErrors::DescriptorInvalid);
        auto prepared = runtime->PrepareWorld(Test::SmallWorldSettings());
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(863).Value()).HasValue());
        Test::RequireError(world->ReadContinuousCollision(), PhysicsErrors::CapabilityUnavailable);
        PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                   .worldGeneration = 863,
                                                   .sceneGeneration = 7,
                                                   .targetKind = PhysicsCommandTargetKind::World,
                                                   .targetIdentity = 863,
                                                   .commandKind = PhysicsStructuralCommandKind::Change,
                                                   .source = PhysicsCommandSourceId::Create(1).Value(),
                                                   .sourceSequence = 1},
                                         .continuousCollision = PhysicsContinuousCollisionPolicy{}};
        Test::RequireError(world->QueueStructuralCommand(command), PhysicsErrors::CapabilityUnavailable);
    }

#if !HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Omitted native solver cannot qualify a CCD tunnelling result", "[physics][ccd][omitted]") {
        Test::RequireError(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical), PhysicsErrors::CapabilityUnavailable);
    }
#endif

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        template <typename Id> Id CollisionId(const std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return Id::FromBytes(bytes);
        }

        PhysicsWorldSimulationBinding SimulationBinding(const SimulationPairResponse response = SimulationPairResponse::Block) {
            const auto layer = CollisionId<CollisionLayerId>(1);
            const auto profile = CollisionId<CollisionProfileId>(2);
            const auto channel = CollisionId<PhysicsQueryChannelId>(3);
            ProjectCollisionSchema authored{.defaultProfile = profile,
                                            .layers = {{layer}},
                                            .pairs = {{layer, layer, response}},
                                            .queryChannels = {{channel}},
                                            .profiles = {{profile, layer, true, true, true, {{channel, CollisionQueryResponse::Block}}}}};
            auto normalized = NormalizedCollisionSchema::Create(authored);
            REQUIRE(normalized.HasValue());
            return {std::make_shared<const NormalizedCollisionSchema>(std::move(normalized).Value()), 1};
        }

        PhysicsSceneCollisionBinding BodyCollision() {
            return {.profile = CollisionId<CollisionProfileId>(2)};
        }

        struct ShotOutcome final {
            PhysicsBodyReconciliation body;
            std::int64_t stepNanoseconds{};
        };

        /** @brief Own one genuinely native scene; all setup and copied assertion evidence lie outside the measured step. */
        struct CcdFixture final {
            std::unique_ptr<PhysicsRuntime> runtime;
            std::unique_ptr<PhysicsWorld> world;
            BodyHandle moving;

            explicit CcdFixture(const PhysicsDefaultMotionQuality mode, const bool sleeping = false,
                                const SimulationPairResponse response = SimulationPairResponse::Block,
                                const bool stationaryOverlap = false) {
                auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
                REQUIRE(created.HasValue());
                runtime = std::move(created).Value();
                PhysicsWorldSettingsDescriptor settings;
                settings.world.gravity = {};
                settings.world.capacity = {16, 32, 16, 4096};
                settings.budgets.maximumContactPairs = 32;
                settings.budgets.maximumContactConstraints = 16;
                settings.budgets.maximumInFlightPairs = 8;
                settings.budgets.scratchBytes = 1024 * 1024;
                settings.step.defaultMotionQuality = mode;
                auto captured = PhysicsWorldSettings::Capture(settings);
                REQUIRE(captured.HasValue());
                auto prepared = runtime->PrepareWorld(captured.Value(), SimulationBinding(response));
                REQUIRE(prepared.HasValue());
                world = std::move(prepared).Value();
                REQUIRE(world->Activate(PhysicsWorldId::Create(863).Value()).HasValue());
                AddThinWall();
                AddMovingBody(sleeping, stationaryOverlap);
            }

            PhysicsBodyReconciliation ReadMovingBody() const {
                const auto observed = world->ReadSceneBodyReconciliation(moving);
                REQUIRE(observed.HasValue());
                return observed.Value();
            }

            PhysicsContinuousCollisionObservation ReadPolicy() const {
                const auto observed = world->ReadContinuousCollision();
                REQUIRE(observed.HasValue());
                return observed.Value();
            }

            void AddThinWall() {
                const auto shape = world->CreateSceneShape(PhysicsBoxShape{{0.025F, 10.0F, 10.0F}});
                REQUIRE(shape.HasValue());
                PhysicsBodyDescriptor wall;
                wall.shape = shape.Value();
                REQUIRE(world->CreateSceneBody({.body = wall, .collision = BodyCollision()}).HasValue());
            }

            void AddMovingBody(const bool sleeping, const bool stationaryOverlap) {
                const auto shape = world->CreateSceneShape(PhysicsBoxShape{{0.1F, 0.1F, 0.1F}});
                REQUIRE(shape.HasValue());
                PhysicsBodyDescriptor descriptor;
                descriptor.shape = shape.Value();
                descriptor.motion = PhysicsMotionType::Dynamic;
                descriptor.mass = PhysicsMass{};
                descriptor.pose.translation.x = stationaryOverlap ? 0.0F : -3.0F;
                descriptor.linearVelocity.x = sleeping || stationaryOverlap ? 0.0F : 400.0F;
                auto body = world->CreateSceneBody(
                    {.body = descriptor,
                     .initialActivity = sleeping ? PhysicsInitialBodyActivity::Sleeping : PhysicsInitialBodyActivity::Awake,
                     .collision = BodyCollision()});
                REQUIRE(body.HasValue());
                moving = body.Value();
            }
        };

        /** @brief Advance exactly one canonical tick through the genuine solver, excluding construction and readback from timing. */
        ShotOutcome Shoot(const PhysicsDefaultMotionQuality mode) {
            CcdFixture fixture(mode);
            const auto begin = std::chrono::steady_clock::now();
            const auto stepped = fixture.world->AdvanceFixedTick(
                {.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)});
            const auto end = std::chrono::steady_clock::now();
            REQUIRE(stepped.HasValue());
            const auto state = fixture.world->ReadSceneBodyReconciliation(fixture.moving);
            REQUIRE(state.HasValue());
            return {state.Value(), std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()};
        }

        PhysicsStructuralCommand WorldPolicyCommand(const std::uint64_t tick, const PhysicsContinuousCollisionPolicy policy) {
            return {.order = {.simulationTick = tick,
                              .worldGeneration = 863,
                              .sceneGeneration = 7,
                              .targetKind = PhysicsCommandTargetKind::World,
                              .targetIdentity = 863,
                              .commandKind = PhysicsStructuralCommandKind::Change,
                              .source = PhysicsCommandSourceId::Create(1).Value(),
                              .sourceSequence = 1},
                    .continuousCollision = policy};
        }
    }  // namespace

    TEST_CASE("Native linear CCD resolves the canonical thin wall that discrete motion tunnels through", "[physics][ccd][native]") {
        const auto discrete = Shoot(PhysicsDefaultMotionQuality::Discrete);
        const auto swept = Shoot(PhysicsDefaultMotionQuality::LinearCast);
        REQUIRE(discrete.body.state.pose.translation.x > 0.2F);
        REQUIRE(swept.body.state.pose.translation.x < 0.15F);
        REQUIRE(swept.body.state.pose.translation.x > -0.3F);
        REQUIRE(swept.body.observedMotionQuality == PhysicsDefaultMotionQuality::LinearCast);
        REQUIRE(discrete.body.observedMotionQuality == PhysicsDefaultMotionQuality::Discrete);
    }

    TEST_CASE("Installed ignore and overlap rows preserve genuine nonblocking simulation response", "[physics][ccd][native][filters]") {
        for (const auto response : {SimulationPairResponse::Ignore, SimulationPairResponse::Overlap}) {
            CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete, false, response);
            REQUIRE(fixture.world
                        ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasValue());
            REQUIRE(fixture.ReadMovingBody().state.pose.translation.x > 0.2F);
        }
    }

    TEST_CASE("Native overlap rows publish real sensor contact evidence with exact scene identity", "[physics][ccd][native][events]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete, false, SimulationPairResponse::Overlap, true);
        auto issued = fixture.world->IssueQueryEventCapability();
        REQUIRE(issued.HasValue());
        auto capability = std::move(issued).Value();
        REQUIRE(fixture.world
                    ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto tick = fixture.world->PublishedTick();
        std::array<PhysicsEventRecord, 4> events{};
        const auto read = capability.ReadEvents({.identity = capability.Identity(),
                                                 .completedTick = tick.eventTick,
                                                 .publicationRevision = tick.publicationRevision,
                                                 .maximumRecords = 4},
                                                events);
        REQUIRE(read.HasValue());
        REQUIRE(read.Value().recordCount > 0);
        REQUIRE((events[0].pair.first.body == fixture.moving || events[0].pair.second.body == fixture.moving));
        REQUIRE(events[0].pair.first.profile == CollisionId<CollisionProfileId>(2));
        REQUIRE(events[0].pair.second.profile == CollisionId<CollisionProfileId>(2));
        REQUIRE(events[0].pair.first.filterSchemaGeneration == 1);
        REQUIRE_FALSE(events[0].pair.first.subshape.has_value());
        REQUIRE_FALSE(events[0].pair.second.subshape.has_value());
        REQUIRE(events[0].kind == PhysicsEventKind::TriggerEnter);
        REQUIRE(fixture.ReadMovingBody().state.pose.translation.x == 0.0F);
    }

    TEST_CASE("Positive CCD and unknown profiles fail before publication in an unbound world", "[physics][ccd][native][binding]") {
        auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        auto prepared = runtime->PrepareWorld(Test::SmallWorldSettings());
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(863).Value()).HasValue());
        const auto shape = world->CreateSceneShape(PhysicsSphereShape{});
        REQUIRE(shape.HasValue());
        PhysicsBodyDescriptor body{.shape = shape.Value(), .motion = PhysicsMotionType::Dynamic, .mass = PhysicsMass{}};
        body.continuousCollision.mode = PhysicsDefaultMotionQuality::LinearCast;
        Test::RequireError(world->CreateSceneBody({.body = body}), PhysicsErrors::CapabilityUnavailable);
        body.continuousCollision.mode = PhysicsDefaultMotionQuality::Discrete;
        Test::RequireError(world->CreateSceneBody({.body = body, .collision = BodyCollision()}), PhysicsErrors::CapabilityUnavailable);
        body.continuousCollision.mode.reset();
        const auto resident = world->CreateSceneBody({.body = body});
        REQUIRE(resident.HasValue());
        Test::RequireError(world->QueueStructuralCommand(WorldPolicyCommand(1, {PhysicsDefaultMotionQuality::LinearCast})),
                           PhysicsErrors::CapabilityUnavailable);
        REQUIRE(world->ReadContinuousCollision().Value().revision == 1);
        REQUIRE(world->ReadSceneBodyReconciliation(resident.Value()).Value().observedMotionQuality ==
                PhysicsDefaultMotionQuality::Discrete);
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(world->ReadContinuousCollision().Value().revision == 1);
        REQUIRE(world->ReadSceneBodyReconciliation(resident.Value()).Value().observedMotionQuality ==
                PhysicsDefaultMotionQuality::Discrete);
    }

    TEST_CASE("Installed simulation rejects unknown body profiles and inconsistent capture generations",
              "[physics][ccd][native][binding]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::LinearCast);
        const auto shape = fixture.world->CreateSceneShape(PhysicsSphereShape{});
        REQUIRE(shape.HasValue());
        PhysicsBodyDescriptor body{.shape = shape.Value(), .motion = PhysicsMotionType::Dynamic, .mass = PhysicsMass{}};
        auto binding = BodyCollision();
        binding.profile = CollisionId<CollisionProfileId>(99);
        Test::RequireError(fixture.world->CreateSceneBody({.body = body, .collision = binding}), PhysicsErrors::DescriptorInvalid);
        binding = BodyCollision();
        binding.colliders.resize(257);
        Test::RequireError(fixture.world->CreateSceneBody({.body = body, .collision = binding}), PhysicsErrors::CapacityExceeded);
        Test::RequireError(fixture.runtime->PrepareWorld(Test::SmallWorldSettings(), {{}, 1}), PhysicsErrors::DescriptorInvalid);
        auto capture = SimulationBinding();
        capture.generation = 0;
        Test::RequireError(fixture.runtime->PrepareWorld(Test::SmallWorldSettings(), capture), PhysicsErrors::DescriptorInvalid);
        REQUIRE(fixture.ReadMovingBody().state.body == fixture.moving);
    }

    TEST_CASE("World CCD changes apply at exact pre-step and wake affected bodies without replacing their handles",
              "[physics][ccd][native][mutation]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::LinearCast, true);
        const auto old = fixture.world->ReadContinuousCollision();
        REQUIRE(old.HasValue());
        REQUIRE(fixture.ReadMovingBody().state.activity == PhysicsBodyActivity::Sleeping);
        PhysicsContinuousCollisionPolicy policy{PhysicsDefaultMotionQuality::LinearCast, 0.5F, 0.1F};
        REQUIRE(fixture.world->QueueStructuralCommand(WorldPolicyCommand(2, policy)).HasValue());
        REQUIRE(fixture.ReadPolicy().revision == old.Value().revision);
        REQUIRE(fixture.world
                    ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(fixture.ReadPolicy().policy == old.Value().policy);
        const auto advanced = fixture.world->AdvanceFixedTick(
            {.simulationTick = 2, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)});
        if (advanced.HasError()) {
            INFO("CCD tick failure domain=" << advanced.ErrorValue().domain.Value() << " code=" << advanced.ErrorValue().code.Value()
                                            << " message=" << advanced.ErrorValue().message);
        }
        REQUIRE(advanced.HasValue());
        const auto current = fixture.world->ReadContinuousCollision();
        REQUIRE(current.HasValue());
        REQUIRE(current.Value().revision == old.Value().revision + 1);
        REQUIRE(current.Value().policy == policy);
        REQUIRE(current.Value().world == fixture.world->Identity());
        REQUIRE(fixture.world->Settings().Values().step.linearCastThresholdFraction == 0.75F);
        const auto body = fixture.world->ReadSceneBodyReconciliation(fixture.moving);
        REQUIRE(body.HasValue());
        REQUIRE(body.Value().state.body == fixture.moving);
        REQUIRE(body.Value().state.activity == PhysicsBodyActivity::Awake);
        REQUIRE(body.Value().observedMotionQuality == PhysicsDefaultMotionQuality::LinearCast);
    }

    TEST_CASE("Native explicit CCD rejects sensors and zero-inner-radius shapes without publishing a body",
              "[physics][ccd][native][admission]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::LinearCast);
        PhysicsBodyDescriptor body;
        const auto shape = fixture.world->CreateSceneShape(PhysicsSphereShape{});
        REQUIRE(shape.HasValue());
        body.shape = shape.Value();
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{};
        body.continuousCollision.mode = PhysicsDefaultMotionQuality::LinearCast;
        Test::RequireError(fixture.world->CreateSceneBody({body, true}), PhysicsErrors::OperationUnsupported);
        const auto plane = fixture.world->CreateSceneShape(PhysicsStaticPlaneShape{});
        REQUIRE(plane.HasValue());
        body.shape = plane.Value();
        Test::RequireError(fixture.world->CreateSceneBody({body, false}), PhysicsErrors::OperationUnsupported);
        body.shape = shape.Value();
        body.continuousCollision.mode.reset();
        const auto inheritedSensor = fixture.world->CreateSceneBody({body, true});
        REQUIRE(inheritedSensor.HasValue());
        const auto observed = fixture.world->ReadSceneBodyReconciliation(inheritedSensor.Value());
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().observedMotionQuality == PhysicsDefaultMotionQuality::Discrete);
    }

    TEST_CASE("Queued body CCD override preserves its identity and applies before a real tunnelling step",
              "[physics][ccd][native][mutation]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete);
        PhysicsStructuralCommand command{.order = {.simulationTick = 1,
                                                   .worldGeneration = 863,
                                                   .sceneGeneration = 7,
                                                   .targetKind = PhysicsCommandTargetKind::Body,
                                                   .targetIdentity = static_cast<std::uint64_t>(fixture.moving.slot.index) + 1,
                                                   .commandKind = PhysicsStructuralCommandKind::Change,
                                                   .source = PhysicsCommandSourceId::Create(1).Value(),
                                                   .sourceSequence = 1},
                                         .bodyMutation = PhysicsBodyMutation{.body = fixture.moving,
                                                                             .continuousCollision = PhysicsBodyContinuousCollision{
                                                                                 PhysicsDefaultMotionQuality::LinearCast}}};
        REQUIRE(fixture.world->QueueStructuralCommand(command).HasValue());
        REQUIRE(fixture.ReadMovingBody().observedMotionQuality == PhysicsDefaultMotionQuality::Discrete);
        REQUIRE(fixture.world
                    ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto observed = fixture.world->ReadSceneBodyReconciliation(fixture.moving);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().state.body == fixture.moving);
        REQUIRE(observed.Value().observedMotionQuality == PhysicsDefaultMotionQuality::LinearCast);
        REQUIRE(observed.Value().state.pose.translation.x < 0.15F);
    }

    TEST_CASE("Invalid or duplicate CCD world commands leave effective policy and native bodies intact",
              "[physics][ccd][native][admission]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete);
        const auto old = fixture.world->ReadContinuousCollision();
        REQUIRE(old.HasValue());
        auto command = WorldPolicyCommand(1, {PhysicsDefaultMotionQuality::LinearCast, 0.5F, 0.25F});
        command.continuousCollision->thresholdFraction = 0.0F;
        Test::RequireError(fixture.world->QueueStructuralCommand(command), PhysicsErrors::DescriptorInvalid);
        command.continuousCollision->thresholdFraction = 0.5F;
        command.order.targetIdentity = 999;
        Test::RequireError(fixture.world->QueueStructuralCommand(command), PhysicsErrors::CommandOrderInvalid);
        command.order.targetIdentity = 863;
        REQUIRE(fixture.world->QueueStructuralCommand(command).HasValue());
        Test::RequireError(fixture.world->QueueStructuralCommand(command), PhysicsErrors::CommandOrderInvalid);
        REQUIRE(fixture.ReadPolicy().policy == old.Value().policy);
        REQUIRE(fixture.ReadMovingBody().observedMotionQuality == PhysicsDefaultMotionQuality::Discrete);
        REQUIRE(fixture.world
                    ->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        REQUIRE(fixture.ReadPolicy().revision == old.Value().revision + 1);
        REQUIRE(fixture.ReadMovingBody().state.pose.translation.x < 0.15F);
    }

    TEST_CASE("A CCD command with a missing per-tick source predecessor preserves the old native policy",
              "[physics][ccd][native][ordering]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete);
        const auto old = fixture.ReadPolicy();
        auto command = WorldPolicyCommand(1, {PhysicsDefaultMotionQuality::LinearCast, 0.5F, 0.25F});
        command.order.sourceSequence = 2;
        REQUIRE(fixture.world->QueueStructuralCommand(command).HasValue());
        Test::RequireError(fixture.world->AdvanceFixedTick(
                               {.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                           PhysicsErrors::CommandOrderInvalid);
        REQUIRE(fixture.ReadPolicy().policy == old.policy);
        REQUIRE(fixture.ReadPolicy().revision == old.revision);
        REQUIRE(fixture.ReadMovingBody().state.pose.translation.x == -3.0F);
        REQUIRE(fixture.world->PublishedTick().publicationRevision == 0);
    }

    TEST_CASE("The complete pre-step frame validates before a CCD world change can publish", "[physics][ccd][native][mutation]") {
        CcdFixture fixture(PhysicsDefaultMotionQuality::Discrete);
        const auto old = fixture.world->ReadContinuousCollision();
        REQUIRE(old.HasValue());
        REQUIRE(fixture.world->QueueStructuralCommand(WorldPolicyCommand(1, {PhysicsDefaultMotionQuality::LinearCast, 0.5F, 0.25F}))
                    .HasValue());
        PhysicsStructuralCommand body{.order = {.simulationTick = 1,
                                                .worldGeneration = 863,
                                                .sceneGeneration = 7,
                                                .targetKind = PhysicsCommandTargetKind::Body,
                                                .targetIdentity = static_cast<std::uint64_t>(fixture.moving.slot.index) + 1,
                                                .commandKind = PhysicsStructuralCommandKind::Change,
                                                .source = PhysicsCommandSourceId::Create(2).Value(),
                                                .sourceSequence = 1},
                                      .bodyMutation = PhysicsBodyMutation{.body = fixture.moving, .mass = PhysicsMass{2.0F}}};
        REQUIRE(fixture.world->QueueStructuralCommand(body).HasValue());
        PhysicsConstraintDescriptor constraint;
        constraint.first = {fixture.moving, {}};
        constraint.second = PhysicsWorldAnchor{};
        constraint.parameters = PhysicsFixedConstraint{};
        REQUIRE(fixture.world->CreateSceneConstraint(constraint).HasValue());
        Test::RequireError(fixture.world->AdvanceFixedTick(
                               {.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                           PhysicsErrors::OperationUnsupported);
        REQUIRE(fixture.ReadPolicy().policy == old.Value().policy);
        REQUIRE(fixture.ReadPolicy().revision == old.Value().revision);
        REQUIRE(fixture.world->PublishedTick().publicationRevision == 0);
        REQUIRE(fixture.ReadMovingBody().state.pose.translation.x == -3.0F);
    }

    TEST_CASE("Repeated CCD thin-wall fixture has exact same-machine diagnostic outcomes and a measured relative step budget",
              "[physics][ccd][native][qualification]") {
        const auto expected = Shoot(PhysicsDefaultMotionQuality::LinearCast);
        std::int64_t discreteTime{}, ccdTime{};
        constexpr std::size_t Samples = 24;
        for (std::size_t index = 0; index < Samples; ++index) {
            const auto discrete = Shoot(PhysicsDefaultMotionQuality::Discrete);
            const auto ccd = Shoot(PhysicsDefaultMotionQuality::LinearCast);
            REQUIRE(ccd.body.state.pose == expected.body.state.pose);
            REQUIRE(ccd.body.state.linearVelocity == expected.body.state.linearVelocity);
            REQUIRE(ccd.body.state.angularVelocity == expected.body.state.angularVelocity);
            REQUIRE(ccd.body.state.activity == expected.body.state.activity);
            discreteTime += discrete.stepNanoseconds;
            ccdTime += ccd.stepNanoseconds;
        }
        REQUIRE(discreteTime > 0);
        const double ratio = static_cast<double>(ccdTime) / static_cast<double>(discreteTime);
        std::cout << "CCD_COST samples=" << Samples << " discrete_ns=" << discreteTime << " ccd_ns=" << ccdTime << " ratio=" << ratio
                  << " budget=32 SameMachineDiagnostic\n";
        REQUIRE(ratio <= 32.0);
    }
#endif
}  // namespace Horo::Physics
