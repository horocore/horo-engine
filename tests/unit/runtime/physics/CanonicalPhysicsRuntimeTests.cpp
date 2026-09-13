#include "CanonicalPhysicsRuntime.h"
#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsTestUtils.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <catch2/catch_test_macros.hpp>
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
        SubmitCanonicalDiagnosticForTesting(world.handle, CanonicalDiagnosticKind::Validation, oversized);
        const auto validated = StepCanonicalWorld(world.handle, 1.0F / 60.0F);
        REQUIRE(validated.HasValue());
        REQUIRE(validated.Value().diagnostic.has_value());
        REQUIRE(validated.Value().diagnostic->code.Value() == PhysicsErrors::SolverValidationMessage.code.Value());
        REQUIRE(validated.Value().diagnostic->severity == ErrorSeverity::Warning);
        REQUIRE(validated.Value().diagnostic->message.size() == MaximumPhysicsDiagnosticMessageBytes);

        SubmitCanonicalDiagnosticForTesting(world.handle, CanonicalDiagnosticKind::Validation, "lower-priority validation");
        SubmitCanonicalDiagnosticForTesting(world.handle, CanonicalDiagnosticKind::Assertion, "bounded solver assertion");
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

        REQUIRE(StepCanonicalWorld({}, 1.0F / 60.0F).ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value());
        SubmitCanonicalDiagnosticForTesting({}, CanonicalDiagnosticKind::Fatal, "ignored after retirement");
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
#ifdef JPH_ENABLE_ASSERTS
            REQUIRE(JPH::AssertFailed != priorAssert);
#endif
        }
        REQUIRE(JPH::Trace == priorTrace);
#ifdef JPH_ENABLE_ASSERTS
        REQUIRE(JPH::AssertFailed == priorAssert);
#endif
    }
}  // namespace Horo::Physics::Detail
