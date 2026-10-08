#include "CanonicalPhysicsRuntime.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsEventProjection.h"
#include "PhysicsTestUtils.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Physics::Detail {
    namespace {
        struct RuntimeOwner final {
            CanonicalRuntimeHandle handle;

            ~RuntimeOwner() {
                DestroyCanonicalRuntime(handle);
            }
        };

        struct WorldOwner final {
            CanonicalWorldHandle handle;

            ~WorldOwner() {
                DestroyCanonicalWorld(handle);
            }
        };

        void RequireUnownedNativeGlobals() {
            REQUIRE(JPH::Factory::sInstance == nullptr);
            REQUIRE(JPH::Allocate == nullptr);
            REQUIRE(JPH::Reallocate == nullptr);
            REQUIRE(JPH::Free == nullptr);
            REQUIRE(JPH::AlignedAllocate == nullptr);
            REQUIRE(JPH::AlignedFree == nullptr);
        }

        bool CaptureProjection(PhysicsEventProjection *projection, const PhysicsContactObservation &observation) noexcept {
            return projection != nullptr && projection->TryCapture(observation);
        }

        [[nodiscard]] PhysicsQueryFixtureDescriptor ContactFixture(const Math::Vec3 translation) {
            constexpr std::array<std::uint8_t, 16> layerBytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            constexpr std::array<std::uint8_t, 16> profileBytes{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
            constexpr std::array<std::uint8_t, 16> channelBytes{3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};
            return {.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                    .pose = {.translation = translation, .rotation = Math::Quaternion::Identity()},
                    .layer = CollisionLayerId::FromBytes(layerBytes),
                    .profile = CollisionProfileId::FromBytes(profileBytes),
                    .channel = PhysicsQueryChannelId::FromBytes(channelBytes),
                    .response = PhysicsQueryFixtureResponse::Block,
                    .trigger = false,
                    .subshape = PhysicsShapeSubresourceId::FromValue(11)};
        }

        PhysicsWorldSimulationBinding ContactSimulation() {
            const auto fixture = ContactFixture({});
            ProjectCollisionSchema
                authored{.defaultProfile = fixture.profile,
                         .layers = {{fixture.layer}},
                         .pairs = {{fixture.layer, fixture.layer, SimulationPairResponse::Block}},
                         .queryChannels = {{fixture.channel}},
                         .profiles = {
                             {fixture.profile, fixture.layer, true, true, true, {{fixture.channel, CollisionQueryResponse::Block}}}}};
            auto normalized = NormalizedCollisionSchema::Create(authored);
            REQUIRE(normalized.HasValue());
            return {std::make_shared<const NormalizedCollisionSchema>(std::move(normalized).Value()), 1};
        }

        void RequireContactAuthority(const CanonicalWorld &canonical) {
            const auto metadata = ContactFixture({});
            REQUIRE(canonical.simulation.Profile(metadata.profile) != nullptr);
            REQUIRE(canonical.simulation.Channel(metadata.channel) != nullptr);
            REQUIRE(canonical.simulation.QueryResponse(metadata.profile, metadata.channel) == CollisionQueryResponse::Block);
            REQUIRE(canonical.simulation.SchemaGeneration() == 1);
            REQUIRE(canonical.simulation.Layer(0) == CollisionLayerId{});
            REQUIRE_FALSE(canonical.simulation.QueryResponse(CollisionProfileId{}, metadata.channel).has_value());
        }

        void RequireResidentBody(CanonicalWorld &canonical, const PhysicsWorldId owner, const JPH::BodyID native,
                                 const BodyHandle expected) {
            const auto access = MakeQueryAccess(canonical);
            const auto *resident = ResolveCanonicalSceneBody(access, owner, native);
            REQUIRE(resident != nullptr);
            REQUIRE(resident->handle == expected);
            REQUIRE(ResolveCanonicalSceneBody(access, PhysicsWorldId::Create(999).Value(), native) == nullptr);
        }

    }  // namespace

    TEST_CASE("Canonical native planes preserve the Horo signed-distance equation", "[physics][native][shape]") {
        const auto created = CreateCanonicalRuntime();
        REQUIRE(created.HasValue());
        const RuntimeOwner runtime{created.Value()};
        for (const float distance : {-1.7F, 1.7F}) {
            const PhysicsStaticPlaneShape descriptor{{0, -1, 0}, distance};
            const auto shape = CreateNativeShape(descriptor);
            REQUIRE(shape.HasValue());
            const auto &plane = static_cast<const JPH::PlaneShape *>(shape.Value().GetPtr())->GetPlane();
            REQUIRE(plane.SignedDistance(JPH::Vec3{0, -distance, 0}) == 0.0F);
            REQUIRE(plane.GetConstant() == -distance);
        }
    }

    TEST_CASE("Canonical runtime and world lifecycle retain no partial native ownership", "[physics][native][lifecycle]") {
        SECTION("process initialization rolls back every completed registration stage") {
            for (const auto point : {CanonicalFailurePoint::AllocatorRegistered, CanonicalFailurePoint::FactoryCreated,
                                     CanonicalFailurePoint::TypesRegistered}) {
                RequireUnownedNativeGlobals();
                const auto failed = CreateCanonicalRuntime(point);
                REQUIRE(failed.HasError());
                REQUIRE(failed.ErrorValue().code.Value() == PhysicsErrors::InitializationFailed.code.Value());
                RequireUnownedNativeGlobals();
            }
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            REQUIRE(JPH::Factory::sInstance != nullptr);
        }

        SECTION("world initialization releases scratch jobs and solver state after each failure") {
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            const auto settings = Test::SmallWorldSettings();
            for (const auto point :
                 {CanonicalFailurePoint::ScratchCreated, CanonicalFailurePoint::JobsCreated, CanonicalFailurePoint::SystemInitialized}) {
                const auto failed = CreateCanonicalWorld(runtime.handle, settings, point);
                REQUIRE(failed.HasError());
                REQUIRE(failed.ErrorValue().code.Value() == PhysicsErrors::InitializationFailed.code.Value());
                REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{});
            }
            const auto firstResult = CreateCanonicalWorld(runtime.handle, settings);
            REQUIRE(firstResult.HasValue());
            const WorldOwner first{firstResult.Value()};
            REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{1, 1, 1, 1});
            {
                const auto secondResult = CreateCanonicalWorld(runtime.handle, settings);
                REQUIRE(secondResult.HasValue());
                const WorldOwner second{secondResult.Value()};
                REQUIRE(first.handle.value != second.handle.value);
                REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{2, 2, 2, 2});
            }
            REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{1, 1, 1, 1});
        }
    }

    TEST_CASE("Canonical world admission validates resources and supports quarantine policy", "[physics][native][lifecycle]") {
        SECTION("world preflight rejects a missing runtime and admits quarantine") {
            const auto settings = Test::SmallWorldSettings();
            REQUIRE(CreateCanonicalWorld({}, settings).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
            REQUIRE(InspectCanonicalResources({}) == CanonicalResourceCounts{});
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            auto descriptor = settings.Values();
            descriptor.nonFinitePolicy = PhysicsNonFinitePolicy::QuarantineBody;
            const auto quarantine = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(quarantine.HasValue());
            const auto admitted = CreateCanonicalWorld(runtime.handle, quarantine.Value());
            REQUIRE(admitted.HasValue());
            DestroyCanonicalWorld(admitted.Value());
            REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{});
        }

        SECTION("worlds reject zero-body capacity before allocation and accept one-body capacity") {
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            auto descriptor = Test::SmallWorldSettings().Values();
            descriptor.world.capacity.maximumBodies = 0;
            const auto settings = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(settings.HasValue());
            const auto rejected = CreateCanonicalWorld(runtime.handle, settings.Value());
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
            REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{});
            descriptor.world.capacity.maximumBodies = 1;
            const auto minimal = PhysicsWorldSettings::Capture(descriptor);
            REQUIRE(minimal.HasValue());
            {
                const auto prepared = CreateCanonicalWorld(runtime.handle, minimal.Value());
                REQUIRE(prepared.HasValue());
                const WorldOwner world{prepared.Value()};
                REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{1, 1, 1, 1});
            }
            REQUIRE(InspectCanonicalResources(runtime.handle) == CanonicalResourceCounts{});
        }
    }

    TEST_CASE("Canonical allocation hooks preserve native allocation semantics", "[physics][native][lifecycle]") {
        SECTION("allocation hooks preserve platform allocation and alignment semantics") {
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            void *memory = JPH::Allocate(16);
            REQUIRE(memory != nullptr);
            memory = JPH::Reallocate(memory, 16, 32);
            REQUIRE(memory != nullptr);
            JPH::Free(memory);
            void *aligned = JPH::AlignedAllocate(64, 32);
            REQUIRE(aligned != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(aligned) % 32 == 0);
            JPH::AlignedFree(aligned);
            void *empty = JPH::Allocate(0);
            REQUIRE(empty != nullptr);
            JPH::Free(empty);
            REQUIRE(CreateCanonicalRuntime().ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        }
    }

    TEST_CASE("Canonical diagnostics normalize bounded validation and fatal assertion evidence", "[physics][native][diagnostics]") {
        const auto created = CreateCanonicalRuntime();
        REQUIRE(created.HasValue());
        const RuntimeOwner runtime{created.Value()};
        const auto prepared = CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings());
        REQUIRE(prepared.HasValue());
        const WorldOwner world{prepared.Value()};

        const std::string oversized(MaximumPhysicsDiagnosticMessageBytes + 128, 'v');
        InvokeCanonicalDiagnosticCallbackForTesting(world.handle, CanonicalDiagnosticKind::Validation, oversized);
        const auto validated = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(validated.HasValue());
        REQUIRE(validated.Value().diagnostic.has_value());
        REQUIRE(validated.Value().diagnostic->code.Value() == PhysicsErrors::SolverValidationMessage.code.Value());
        REQUIRE(validated.Value().diagnostic->severity == ErrorSeverity::Warning);
        REQUIRE(validated.Value().diagnostic->message.size() == MaximumPhysicsDiagnosticMessageBytes);

        SubmitCanonicalDiagnosticForTesting(world.handle, CanonicalDiagnosticKind::Validation, "lower-priority validation");
        InvokeCanonicalDiagnosticCallbackForTesting(world.handle, CanonicalDiagnosticKind::Assertion, "bounded solver assertion");
        const auto asserted = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(asserted.HasError());
        REQUIRE(asserted.ErrorValue().code.Value() == PhysicsErrors::SolverAssertionFailed.code.Value());
        REQUIRE(asserted.ErrorValue().severity == ErrorSeverity::Critical);
        REQUIRE(asserted.ErrorValue().message == "bounded solver assertion");

        std::vector<std::thread> callbackThreads;
        for (std::size_t index = 0; index < 16; ++index) {
            callbackThreads.emplace_back([handle = world.handle, index] {
                const auto kind = index == 15 ? CanonicalDiagnosticKind::Fatal : CanonicalDiagnosticKind::Validation;
                SubmitCanonicalDiagnosticForTesting(handle, kind, "concurrent native callback evidence");
            });
        }
        for (auto &thread : callbackThreads)
            thread.join();
        const auto fatal = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(fatal.HasError());
        REQUIRE(fatal.ErrorValue().code.Value() == PhysicsErrors::SolverFatalCondition.code.Value());

        SubmitCanonicalDiagnosticForTesting(world.handle, CanonicalDiagnosticKind::Validation, {});
        const auto empty = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(empty.HasValue());
        REQUIRE(empty.Value().diagnostic.has_value());
        REQUIRE_FALSE(empty.Value().diagnostic->message.empty());

        SubmitCanonicalDiagnosticForTesting(world.handle, static_cast<CanonicalDiagnosticKind>(255), "unknown classification");
        const auto unknown = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(unknown.HasError());
        REQUIRE(unknown.ErrorValue().code.Value() == PhysicsErrors::SolverFatalCondition.code.Value());

        REQUIRE(StepCanonicalWorld({}, 1.0F / 60.0F).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        SubmitCanonicalDiagnosticForTesting({}, CanonicalDiagnosticKind::Fatal, "ignored after retirement");
        InvokeCanonicalDiagnosticCallbackForTesting({}, CanonicalDiagnosticKind::Validation, "ignored after retirement");
    }

    TEST_CASE("Canonical contact callbacks copy stable evidence before native route retirement", "[physics][native][events]") {
        const auto created = CreateCanonicalRuntime();
        REQUIRE(created.HasValue());
        const RuntimeOwner runtime{created.Value()};
        const auto prepared = CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings());
        REQUIRE(prepared.HasValue());
        const WorldOwner world{prepared.Value()};

        auto firstDescriptor = ContactFixture({0.0F, 0.0F, 0.0F});
        firstDescriptor.material = PhysicsQueryMaterial{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 4,
                                                        PhysicsMaterialSlotId::FromValue(7)};
        const auto first = CreateCanonicalQueryFixture(world.handle, PhysicsWorldId::Create(901).Value(), firstDescriptor);
        const auto second =
            CreateCanonicalQueryFixture(world.handle, PhysicsWorldId::Create(901).Value(), ContactFixture({2.0F, 0.0F, 0.0F}));
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());

        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);
        const CanonicalContactSink sink{.context = &projection, .append = CaptureProjection};
        projection.BeginTick(1);
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first.Value(), second.Value(), 1, sink, {}));
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first.Value(), second.Value(), 1, sink,
                                                         CanonicalContactTestOptions{.persisted = true}));
        const auto completed = projection.CompleteTick(1);
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().publishedRecordCount == 1);

        const auto &record = projection.PublishedEvents().front();
        REQUIRE(record.kind == PhysicsEventKind::ContactBegin);
        REQUIRE(record.pair.first.body == first.Value().body);
        REQUIRE(record.pair.second.body == second.Value().body);
        REQUIRE(record.firstMaterial.has_value());
        REQUIRE(record.firstMaterial->assetGeneration == 4);
        REQUIRE(record.firstMaterial->slot == PhysicsMaterialSlotId::FromValue(7));
        REQUIRE_FALSE(record.secondMaterial.has_value());
        REQUIRE(record.contact.pointCount == 1);
        REQUIRE(record.contact.omittedPointCount == 0);
        REQUIRE(record.contact.points[0].positionOnFirst == Math::Vec3{0.0F, 0.0F, 0.0F});
        REQUIRE(record.contact.points[0].positionOnSecond == Math::Vec3{0.0F, 0.0F, 0.0F});
        REQUIRE(record.contact.points[0].normal == Math::Vec3{0.0F, 1.0F, 0.0F});
        REQUIRE(record.contact.points[0].penetrationDepthMeters == 0.1F);
        REQUIRE_FALSE(record.contact.points[0].normalImpulseEstimateNewtonSeconds.has_value());
    }

    TEST_CASE("Canonical contact callbacks select a bounded deterministic manifold prefix", "[physics][native][events][manifold]") {
        const RuntimeOwner runtime{CreateCanonicalRuntime().Value()};
        const WorldOwner world{CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings()).Value()};
        const auto owner = PhysicsWorldId::Create(902).Value();
        const auto first = CreateCanonicalQueryFixture(world.handle, owner, ContactFixture({0.0F, 0.0F, 0.0F})).Value();
        const auto second = CreateCanonicalQueryFixture(world.handle, owner, ContactFixture({2.0F, 0.0F, 0.0F})).Value();
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);
        const CanonicalContactSink sink{.context = &projection, .append = CaptureProjection};

        projection.BeginTick(1);
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first, second, 1, sink,
                                                         CanonicalContactTestOptions{.contactPointCount = 6}));
        REQUIRE(projection.CompleteTick(1).HasValue());
        const auto &contact = projection.PublishedEvents().front().contact;
        REQUIRE(contact.pointCount == MaximumPhysicsContactPoints);
        REQUIRE(contact.omittedPointCount == 2);
        for (std::uint32_t index = 0; index < contact.pointCount; ++index) {
            REQUIRE(contact.points[index].positionOnFirst.x == static_cast<float>(index));
            REQUIRE(contact.points[index].positionOnSecond.x == static_cast<float>(index));
            REQUIRE_FALSE(contact.points[index].normalImpulseEstimateNewtonSeconds.has_value());
        }
        REQUIRE_FALSE(InvokeCanonicalContactCallbackForTesting(world.handle, first, second, 2, sink,
                                                               CanonicalContactTestOptions{.contactPointCount = 0}));
        REQUIRE_FALSE(InvokeCanonicalContactCallbackForTesting(world.handle, first, second, 2, sink,
                                                               CanonicalContactTestOptions{.contactPointCount = 65}));
    }

    TEST_CASE("Canonical compound callback copies stable child identity and material", "[physics][native][events][compound]") {
        const auto created = CreateCanonicalRuntime();
        REQUIRE(created.HasValue());
        const RuntimeOwner runtime{created.Value()};
        const auto prepared = CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings());
        REQUIRE(prepared.HasValue());
        const WorldOwner world{prepared.Value()};
        const auto identity = PhysicsWorldId::Create(902).Value();
        auto descriptor = ContactFixture({0, 0, 0});
        descriptor.subshape.reset();
        const auto material = PhysicsQueryMaterial{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 9,
                                                   PhysicsMaterialSlotId::FromValue(3)};
        const PhysicsCompoundChild child{.geometry = PhysicsSphereShape{0.5F},
                                         .subshape = PhysicsShapeSubresourceId::FromValue(77),
                                         .material = material,
                                         .layer = descriptor.layer,
                                         .profile = descriptor.profile,
                                         .channel = descriptor.channel};
        descriptor.shape = PhysicsCompoundShapeDescriptor{{child}};
        const auto first = CreateCanonicalQueryFixture(world.handle, identity, descriptor);
        const auto second = CreateCanonicalQueryFixture(world.handle, identity, ContactFixture({2, 0, 0}));
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        auto &nativeWorld = *static_cast<CanonicalWorld *>(world.handle.value);
        const auto &nativeFixture = nativeWorld.query.fixtures.front();
        REQUIRE(ResolveCanonicalFixtureChild(nativeFixture, JPH::SubShapeID{}) != nullptr);
        PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);
        projection.BeginTick(1);
        const CanonicalContactSink sink{.context = &projection, .append = CaptureProjection};
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first.Value(), second.Value(), 1, sink,
                                                         CanonicalContactTestOptions{.contactPointCount = 6}));
        REQUIRE(projection.CompleteTick(1).Value().publishedRecordCount == 1);
        const auto &event = projection.PublishedEvents().front();
        REQUIRE(event.pair.first.subshape == child.subshape);
        REQUIRE(event.firstMaterial.has_value());
        REQUIRE(event.firstMaterial->assetGeneration == material.assetGeneration);
        REQUIRE(event.firstMaterial->slot == material.slot);
        REQUIRE(event.contact.pointCount == MaximumPhysicsContactPoints);
        REQUIRE(event.contact.omittedPointCount == 2);
        REQUIRE(event.contact.points[0].positionOnFirst == Math::Vec3{0.0F, 0.0F, 0.0F});
    }

    TEST_CASE("Canonical diagnostic callbacks are restored after runtime shutdown", "[physics][native][diagnostics][shutdown]") {
        const JPH::TraceFunction priorTrace = JPH::Trace;
#ifdef JPH_ENABLE_ASSERTS
        const JPH::AssertFailedFunction priorAssert = JPH::AssertFailed;
#endif
        {
            const auto created = CreateCanonicalRuntime();
            REQUIRE(created.HasValue());
            const RuntimeOwner runtime{created.Value()};
            REQUIRE(JPH::Trace != priorTrace);
            JPH::Trace(nullptr);
#ifdef JPH_ENABLE_ASSERTS
            REQUIRE(JPH::AssertFailed != priorAssert);
            REQUIRE_FALSE(JPH::AssertFailed(nullptr, nullptr, nullptr, 0));
#endif
        }
        REQUIRE(JPH::Trace == priorTrace);
#ifdef JPH_ENABLE_ASSERTS
        REQUIRE(JPH::AssertFailed == priorAssert);
#endif
    }

    TEST_CASE("Scene and query resources never alias and resident indices retire before native reuse", "[physics][native][identity]") {
        const RuntimeOwner runtime{CreateCanonicalRuntime().Value()};
        const WorldOwner world{
            CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings(), CanonicalFailurePoint::None, ContactSimulation()).Value()};
        const auto owner = PhysicsWorldId::Create(603).Value();
        const auto shape = CreateCanonicalSceneShape(world.handle, owner, PhysicsBoxShape{});
        REQUIRE(shape.HasValue());
        PhysicsBodyDescriptor descriptor{.shape = shape.Value()};
        const auto first =
            CreateCanonicalSceneBody(world.handle, owner,
                                     {.body = descriptor, .collision = PhysicsSceneCollisionBinding{ContactFixture({}).profile}});
        REQUIRE(first.HasValue());
        descriptor.pose.translation.x = 2.0F;
        const auto second =
            CreateCanonicalSceneBody(world.handle, owner,
                                     {.body = descriptor, .collision = PhysicsSceneCollisionBinding{ContactFixture({}).profile}});
        REQUIRE(second.HasValue());
        const auto fixture = CreateCanonicalQueryFixture(world.handle, owner, ContactFixture({0, 0, 4}));
        REQUIRE(fixture.HasValue());
        REQUIRE(fixture.Value().body != first.Value());
        REQUIRE(fixture.Value().body != second.Value());
        REQUIRE(fixture.Value().shape != shape.Value());
        auto &canonical = *static_cast<CanonicalWorld *>(world.handle.value);
        RequireContactAuthority(canonical);
        const auto retiredNative = canonical.scene.bodies.front().nativeBody;
        const auto remainingNative = canonical.scene.bodies.back().nativeBody;
        REQUIRE(canonical.scene.nativeBodyIndices[retiredNative.GetIndex()] == 0);
        REQUIRE(canonical.scene.nativeBodyIndices[remainingNative.GetIndex()] == 1);
        RequireResidentBody(canonical, owner, remainingNative, second.Value());
        QuarantineCanonicalSceneBody(world.handle, first.Value(), {});
        REQUIRE(canonical.scene.nativeBodyIndices[retiredNative.GetIndex()] == std::numeric_limits<std::size_t>::max());
        REQUIRE(canonical.scene.nativeBodyIndices[remainingNative.GetIndex()] == 0);
        REQUIRE(canonical.scene.bodies.front().handle == second.Value());
        {
            const auto access = MakeQueryAccess(canonical);
            REQUIRE(ResolveCanonicalSceneBody(access, owner, retiredNative) == nullptr);
            RequireResidentBody(canonical, owner, remainingNative, second.Value());
        }
        const auto replacement =
            CreateCanonicalSceneBody(world.handle, owner,
                                     {.body = descriptor, .collision = PhysicsSceneCollisionBinding{ContactFixture({}).profile}});
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value() != first.Value());
        REQUIRE(replacement.Value().slot.index > fixture.Value().body.slot.index);
        REQUIRE(ReadCanonicalSceneBodyReconciliation(world.handle, owner, first.Value()).HasError());
        REQUIRE(DestroyCanonicalQueryFixture(world.handle, fixture.Value()).HasValue());
    }

    TEST_CASE("Canonical joint collision suppression lasts until the final pair joint retires", "[physics][native][constraint]") {
        const RuntimeOwner runtime{CreateCanonicalRuntime().Value()};
        const WorldOwner world{
            CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings(), CanonicalFailurePoint::None, ContactSimulation()).Value()};
        const auto owner = PhysicsWorldId::Create(601).Value();
        const ShapeHandle shape = CreateCanonicalSceneShape(world.handle, owner, PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor body;
        body.shape = shape;
        body.motion = PhysicsMotionType::Static;
        body.mass = PhysicsNoMass{};
        const BodyHandle first =
            CreateCanonicalSceneBody(world.handle, owner,
                                     {.body = body, .collision = PhysicsSceneCollisionBinding{ContactFixture({}).profile}})
                .Value();
        body.pose.translation = {2.0F, 0.0F, 0.0F};
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{1.0F};
        const BodyHandle second =
            CreateCanonicalSceneBody(world.handle, owner,
                                     {.body = body, .collision = PhysicsSceneCollisionBinding{ContactFixture({}).profile}})
                .Value();
        auto &canonical = *static_cast<CanonicalWorld *>(world.handle.value);
        const auto contactPolicy = [&] {
            JPH::BodyLockRead firstLock(canonical.native.system->GetBodyLockInterfaceNoLock(), canonical.scene.bodies[0].nativeBody);
            JPH::BodyLockRead secondLock(canonical.native.system->GetBodyLockInterfaceNoLock(), canonical.scene.bodies[1].nativeBody);
            REQUIRE(firstLock.Succeeded());
            REQUIRE(secondLock.Succeeded());
            return canonical.contactListener.OnContactValidate(firstLock.GetBody(), secondLock.GetBody(), JPH::RVec3::sZero(),
                                                               JPH::CollideShapeResult{});
        };
        REQUIRE(contactPolicy() == JPH::ValidateResult::AcceptAllContactsForThisBodyPair);

        PhysicsConstraintDescriptor descriptor;
        descriptor.first = {first, {}};
        descriptor.second = PhysicsBodyAnchor{second, {}};
        descriptor.parameters = PhysicsDistanceConstraint{1.0F, 3.0F};
        const ConstraintHandle distance = CreateCanonicalSceneConstraint(world.handle, owner, descriptor).Value();
        descriptor.parameters = PhysicsFixedConstraint{};
        const ConstraintHandle fixed = CreateCanonicalSceneConstraint(world.handle, owner, descriptor).Value();
        REQUIRE(contactPolicy() == JPH::ValidateResult::RejectAllContactsForThisBodyPair);
        REQUIRE(DestroyCanonicalSceneConstraint(world.handle, distance).HasValue());
        REQUIRE(contactPolicy() == JPH::ValidateResult::RejectAllContactsForThisBodyPair);
        REQUIRE(DestroyCanonicalSceneConstraint(world.handle, fixed).HasValue());
        REQUIRE(contactPolicy() == JPH::ValidateResult::AcceptAllContactsForThisBodyPair);
    }

    TEST_CASE("Canonical single-axis joints preserve hard limits and signed runtime coordinates", "[physics][native][constraint]") {
        const RuntimeOwner runtime{CreateCanonicalRuntime().Value()};
        const WorldOwner world{CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings()).Value()};
        const auto owner = PhysicsWorldId::Create(602).Value();
        const ShapeHandle shape = CreateCanonicalSceneShape(world.handle, owner, PhysicsBoxShape{}).Value();
        PhysicsBodyDescriptor body;
        body.shape = shape;
        body.motion = PhysicsMotionType::Static;
        body.mass = PhysicsNoMass{};
        const BodyHandle first = CreateCanonicalSceneBody(world.handle, owner, {body, false}).Value();
        body.pose.translation = {2.0F, 0.0F, 0.0F};
        body.motion = PhysicsMotionType::Dynamic;
        body.mass = PhysicsMass{1.0F};
        const BodyHandle second = CreateCanonicalSceneBody(world.handle, owner, {body, false}).Value();
        auto &canonical = *static_cast<CanonicalWorld *>(world.handle.value);

        PhysicsConstraintDescriptor descriptor;
        descriptor.first = {first, {}};
        descriptor.second = PhysicsBodyAnchor{second, {}};
        descriptor.parameters = PhysicsHingeConstraint{-0.5F, 0.75F};
        const ConstraintHandle hinge = CreateCanonicalSceneConstraint(world.handle, owner, descriptor).Value();
        const auto *nativeHinge = static_cast<const JPH::HingeConstraint *>(canonical.scene.constraints.back().constraint.GetPtr());
        REQUIRE(nativeHinge->HasLimits());
        REQUIRE(nativeHinge->GetLimitsMin() == -0.5F);
        REQUIRE(nativeHinge->GetLimitsMax() == 0.75F);

        descriptor.parameters = PhysicsSliderConstraint{-3.0F, 4.0F};
        const ConstraintHandle slider = CreateCanonicalSceneConstraint(world.handle, owner, descriptor).Value();
        const auto *nativeSlider = static_cast<const JPH::SliderConstraint *>(canonical.scene.constraints.back().constraint.GetPtr());
        REQUIRE(nativeSlider->HasLimits());
        REQUIRE(nativeSlider->GetLimitsMin() == -3.0F);
        REQUIRE(nativeSlider->GetLimitsMax() == 4.0F);
        REQUIRE(ReadCanonicalSceneJointState(world.handle, slider).Value().coordinate == 2.0F);

        canonical.native.system->GetBodyInterface().SetPosition(canonical.scene.bodies[1].nativeBody, JPH::RVec3{-2.0F, 0.0F, 0.0F},
                                                                JPH::EActivation::DontActivate);
        const auto negative = ReadCanonicalSceneJointState(world.handle, slider);
        REQUIRE(negative.HasValue());
        REQUIRE(negative.Value().kind == PhysicsJointCoordinateKind::PositionMeters);
        REQUIRE(negative.Value().coordinate == -2.0F);
        canonical.native.system->GetBodyInterface().SetRotation(canonical.scene.bodies[1].nativeBody,
                                                                JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.25F),
                                                                JPH::EActivation::DontActivate);
        REQUIRE(std::abs(ReadCanonicalSceneJointState(world.handle, hinge).Value().coordinate - 0.25F) < 0.0001F);
        canonical.native.system->GetBodyInterface().SetRotation(canonical.scene.bodies[1].nativeBody,
                                                                JPH::Quat::sRotation(JPH::Vec3::sAxisY(), -0.25F),
                                                                JPH::EActivation::DontActivate);
        REQUIRE(std::abs(ReadCanonicalSceneJointState(world.handle, hinge).Value().coordinate + 0.25F) < 0.0001F);
        REQUIRE(DestroyCanonicalSceneConstraint(world.handle, hinge).HasValue());
        REQUIRE(DestroyCanonicalSceneConstraint(world.handle, slider).HasValue());
    }
}  // namespace Horo::Physics::Detail
