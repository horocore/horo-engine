#include "Horo/Physics/PhysicsIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        PhysicsWorldId World(const std::uint64_t value) {
            const auto result = PhysicsWorldId::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void ExpectError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
            REQUIRE(result.ErrorValue().domain.Value() == expected.domain.Value());
            REQUIRE_FALSE(result.ErrorValue().message.empty());
        }

        template <typename Handle> void CheckOwnerValidation() {
            const auto world = World(7);
            Handle handle{world, {0, 1}};
            REQUIRE(handle.IsValid());
            REQUIRE(ValidatePhysicsHandleOwner(handle, world).HasValue());
            ExpectError(ValidatePhysicsHandleOwner(handle, {}), PhysicsErrors::WorldInvalid);
            ExpectError(ValidatePhysicsHandleOwner(Handle{}, world), PhysicsErrors::HandleMalformed);
            ExpectError(ValidatePhysicsHandleOwner(handle, World(8)), PhysicsErrors::HandleWorldMismatch);
            handle.world = {};
            REQUIRE_FALSE(handle.IsValid());
            ExpectError(ValidatePhysicsHandleOwner(handle, world), PhysicsErrors::HandleMalformed);
            handle.world = world;
            handle.slot.index = decltype(handle.slot)::InvalidIndex;
            REQUIRE_FALSE(handle.IsValid());
            ExpectError(ValidatePhysicsHandleOwner(handle, world), PhysicsErrors::HandleMalformed);
            handle.slot.index = 0;
            handle.slot.generation = 0;
            REQUIRE_FALSE(handle.IsValid());
            ExpectError(ValidatePhysicsHandleOwner(handle, world), PhysicsErrors::HandleMalformed);
        }

        template <typename Handle> void CheckExactIdentity() {
            const auto world = World(7);
            const Handle original{world, {0, 1}};
            REQUIRE(original == original);
            REQUIRE((original != Handle{World(8), {0, 1}}));
            REQUIRE((original != Handle{world, {1, 1}}));
            REQUIRE((original != Handle{world, {0, 2}}));
            const Handle upper{world, {decltype(original.slot)::InvalidIndex - 1, std::numeric_limits<std::uint32_t>::max()}};
            REQUIRE(upper.IsValid());
            // Shape/owner validation is deliberately not a registry capacity or residency query.
            REQUIRE(ValidatePhysicsHandleOwner(upper, world).HasValue());
            static_assert(std::is_trivially_copyable_v<Handle>);
            static_assert(std::is_trivially_destructible_v<Handle>);
        }

        TEST_CASE("Physics world identities validate host values without inventing a generation", "[physics][identity]") {
            const PhysicsWorldId empty;
            REQUIRE_FALSE(empty.IsValid());
            REQUIRE(empty.Value() == 0);
            const auto invalid = PhysicsWorldId::Create(0);
            REQUIRE(invalid.HasError());
            REQUIRE(invalid.ErrorValue().code.Value() == PhysicsErrors::WorldInvalid.code.Value());
            const auto first = World(1);
            REQUIRE(first.IsValid());
            REQUIRE(first.Value() == 1);
            REQUIRE(first == World(1));
            REQUIRE(first != World(2));
            const auto last = World(std::numeric_limits<std::uint64_t>::max());
            REQUIRE(last.Value() == std::numeric_limits<std::uint64_t>::max());
            REQUIRE(last > first);
            static_assert(std::is_trivially_copyable_v<PhysicsWorldId>);
            static_assert(!std::is_convertible_v<std::uint64_t, PhysicsWorldId>);
        }

        TEST_CASE("Physics handles reject malformed and cross-world values for every registry kind", "[physics][identity]") {
            CheckOwnerValidation<BodyHandle>();
            CheckOwnerValidation<ShapeHandle>();
            CheckOwnerValidation<ConstraintHandle>();
            static_assert(!std::is_convertible_v<BodyHandle, ShapeHandle>);
            static_assert(!std::is_convertible_v<ShapeHandle, ConstraintHandle>);
            static_assert(!std::is_convertible_v<ConstraintHandle, BodyHandle>);
        }

        TEST_CASE("Physics handle equality includes world slot and generation exactly", "[physics][identity]") {
            CheckExactIdentity<BodyHandle>();
            CheckExactIdentity<ShapeHandle>();
            CheckExactIdentity<ConstraintHandle>();
        }

        TEST_CASE("Physics generation replacement never aliases a previous world's identical slot", "[physics][identity]") {
            const BodyHandle old{World(10), {5, 3}};
            const BodyHandle replacement{World(11), old.slot};
            REQUIRE(old != replacement);
            ExpectError(ValidatePhysicsHandleOwner(old, replacement.world), PhysicsErrors::HandleWorldMismatch);
            REQUIRE(ValidatePhysicsHandleOwner(replacement, replacement.world).HasValue());
            REQUIRE(old.world.Value() == 10);
            REQUIRE(old.slot.index == 5);
            REQUIRE(old.slot.generation == 3);
        }

        TEST_CASE("Physics errors have unique definitive identities and actionable diagnostics", "[physics][errors]") {
            const std::array<std::pair<const ErrorCodeDescriptor *, std::string_view>, 17> cases{{
                {&PhysicsErrors::WorldInvalid, "physics.world.invalid"},
                {&PhysicsErrors::HandleMalformed, "physics.handle.malformed"},
                {&PhysicsErrors::HandleWorldMismatch, "physics.handle.world_mismatch"},
                {&PhysicsErrors::HandleStale, "physics.handle.stale"},
                {&PhysicsErrors::GenerationExhausted, "physics.generation.exhausted"},
                {&PhysicsErrors::CapabilityUnavailable, "physics.capability.unavailable"},
                {&PhysicsErrors::OperationUnsupported, "physics.operation.unsupported"},
                {&PhysicsErrors::TransformAuthorityViolation, "physics.transform.authority_violation"},
                {&PhysicsErrors::InvalidState, "physics.state.invalid"},
                {&PhysicsErrors::SolverValidationMessage, "physics.solver.validation"},
                {&PhysicsErrors::SolverAssertionFailed, "physics.solver.assertion_failed"},
                {&PhysicsErrors::SolverFatalCondition, "physics.solver.fatal_condition"},
                {&PhysicsErrors::DescriptorInvalid, "physics.descriptor.invalid"},
                {&PhysicsErrors::ProfileUnsupported, "physics.profile.unsupported"},
                {&PhysicsErrors::CapacityExceeded, "physics.capacity.exceeded"},
                {&PhysicsErrors::CapabilityStale, "physics.capability.stale"},
                {&PhysicsErrors::InitializationFailed, "physics.initialization.failed"},
            }};
            std::set<std::string_view> unique;
            for (const auto &[descriptor, code] : cases) {
                REQUIRE(descriptor->domain.Value() == "horo.physics");
                REQUIRE(descriptor->code.Value() == code);
                REQUIRE(unique.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
                REQUIRE(descriptor->retryable == (descriptor == &PhysicsErrors::CapabilityStale));
                const auto error = MakeError(*descriptor);
                REQUIRE(error.code.Value() == code);
                REQUIRE(error.severity == descriptor->defaultSeverity);
                REQUIRE(error.message == descriptor->summary);
            }
            REQUIRE(PhysicsErrors::GenerationExhausted.defaultSeverity == ErrorSeverity::Critical);
            REQUIRE(PhysicsErrors::SolverAssertionFailed.defaultSeverity == ErrorSeverity::Critical);
            REQUIRE(PhysicsErrors::SolverFatalCondition.defaultSeverity == ErrorSeverity::Critical);
            REQUIRE(PhysicsErrors::CapabilityUnavailable.userActionable);
            REQUIRE(PhysicsErrors::OperationUnsupported.userActionable);
        }
    }  // namespace
}  // namespace Horo::Physics
