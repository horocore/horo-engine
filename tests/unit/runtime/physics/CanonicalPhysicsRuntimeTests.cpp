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
    }  // namespace

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

    TEST_CASE("Canonical world admission rejects unsupported resource policies before allocation", "[physics][native][lifecycle]") {
        SECTION("world preflight rejects missing runtime and unimplemented containment before allocation") {
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
            const auto rejected = CreateCanonicalWorld(runtime.handle, quarantine.Value());
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
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
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first.Value(), second.Value(), 1, false, false, sink));
        REQUIRE(InvokeCanonicalContactCallbackForTesting(world.handle, first.Value(), second.Value(), 1, false, true, sink));
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
        REQUIRE(record.contact.position == Math::Vec3{0.0F, 0.0F, 0.0F});
        REQUIRE(record.contact.normal == Math::Vec3{0.0F, 1.0F, 0.0F});
        REQUIRE(record.contact.penetrationDepthMeters == 0.1F);
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

    TEST_CASE("Canonical joint collision suppression lasts until the final pair joint retires", "[physics][native][constraint]") {
        const RuntimeOwner runtime{CreateCanonicalRuntime().Value()};
        const WorldOwner world{CreateCanonicalWorld(runtime.handle, Test::SmallWorldSettings()).Value()};
        const auto owner = PhysicsWorldId::Create(601).Value();
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
