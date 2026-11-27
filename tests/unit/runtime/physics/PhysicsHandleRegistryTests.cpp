#include "PhysicsTestUtils.h"
#include "model/identity/PhysicsHandleRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Physics {
    namespace {
        struct NativeBodyId final {
            std::uint32_t value{};
        };

        struct NativeShapeId final {
            std::uint32_t value{};
        };

        struct NativeConstraintId final {
            std::uint32_t value{};
        };

        template <typename Handle, typename Value> using Registry = Detail::PhysicsHandleRegistry<Handle, Value>;

        PhysicsWorldId World(const std::uint64_t value) {
            return PhysicsWorldId::Create(value).Value();
        }

        using Test::RequireError;

        TEST_CASE("Physics registries keep body shape and constraint mappings strongly typed", "[physics][registry]") {
            auto bodies = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1}).Value();
            auto shapes = Registry<ShapeHandle, NativeShapeId>::Create({.maximumSlots = 1}).Value();
            auto constraints = Registry<ConstraintHandle, NativeConstraintId>::Create({.maximumSlots = 1}).Value();
            const auto world = World(41);
            REQUIRE(bodies.BindOwner(world).HasValue());
            REQUIRE(shapes.BindOwner(world).HasValue());
            REQUIRE(constraints.BindOwner(world).HasValue());

            const auto body = bodies.Acquire({11}).Value();
            const auto shape = shapes.Acquire({22}).Value();
            const auto constraint = constraints.Acquire({33}).Value();
            REQUIRE(body.world == world);
            REQUIRE(body.slot.index == 0);
            REQUIRE(body.slot.generation == 1);
            REQUIRE(shapes.Resolve(shape).Value()->value == 22);
            REQUIRE(constraints.Resolve(constraint).Value()->value == 33);
            REQUIRE(bodies.Resolve(body).Value()->value == 11);
            static_assert(!std::is_same_v<decltype(body), decltype(shape)>);
            static_assert(!std::is_same_v<decltype(shape), decltype(constraint)>);
        }

        TEST_CASE("Physics registry owner binding is allocation-free state and happens once", "[physics][registry]") {
            auto registry = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1}).Value();
            REQUIRE_FALSE(registry.IsBound());
            RequireError(registry.Acquire({1}), PhysicsErrors::InvalidState);
            RequireError(registry.Resolve(BodyHandle{World(51), {0, 1}}), PhysicsErrors::InvalidState);
            RequireError(registry.BindOwner({}), PhysicsErrors::WorldInvalid);
            REQUIRE(registry.BindOwner(World(51)).HasValue());
            REQUIRE(registry.IsBound());
            RequireError(registry.BindOwner(World(52)), PhysicsErrors::InvalidState);
        }

        TEST_CASE("Physics registry move transfers ownership and leaves the source inert", "[physics][registry]") {
            auto source = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1}).Value();
            REQUIRE(source.BindOwner(World(56)).HasValue());
            const auto handle = source.Acquire({19}).Value();

            auto destination = std::move(source);
            REQUIRE_FALSE(source.IsBound());
            REQUIRE(source.Capacity() == 0);
            REQUIRE(source.ActiveCount() == 0);
            RequireError(source.Resolve(handle), PhysicsErrors::InvalidState);
            REQUIRE(destination.IsBound());
            REQUIRE(destination.ActiveCount() == 1);
            REQUIRE(destination.Resolve(handle).Value()->value == 19);

            auto reassigned = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 2}).Value();
            reassigned = std::move(destination);
            REQUIRE_FALSE(destination.IsBound());
            REQUIRE(destination.Capacity() == 0);
            REQUIRE(destination.ActiveCount() == 0);
            RequireError(destination.Resolve(handle), PhysicsErrors::InvalidState);
            REQUIRE(reassigned.IsBound());
            REQUIRE(reassigned.Capacity() == 1);
            REQUIRE(reassigned.Resolve(handle).Value()->value == 19);
        }

        TEST_CASE("Physics registries reject identical foreign-world slots before private lookup", "[physics][registry]") {
            auto first = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1}).Value();
            auto second = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1}).Value();
            REQUIRE(first.BindOwner(World(61)).HasValue());
            REQUIRE(second.BindOwner(World(62)).HasValue());
            const auto firstHandle = first.Acquire({7}).Value();
            const auto secondHandle = second.Acquire({8}).Value();
            REQUIRE(firstHandle.slot == secondHandle.slot);
            RequireError(second.Resolve(firstHandle), PhysicsErrors::HandleWorldMismatch);
            RequireError(second.Remove(firstHandle), PhysicsErrors::HandleWorldMismatch);
            REQUIRE(second.ActiveCount() == 1);
            REQUIRE(second.Resolve(secondHandle).Value()->value == 8);
        }

        TEST_CASE("Physics slot reuse invalidates every older generation", "[physics][registry]") {
            auto registry = Registry<ShapeHandle, NativeShapeId>::Create({.maximumSlots = 1}).Value();
            REQUIRE(registry.BindOwner(World(71)).HasValue());
            const auto first = registry.Acquire({17}).Value();
            REQUIRE(registry.Remove(first).HasValue());
            REQUIRE(registry.ActiveCount() == 0);
            RequireError(registry.Resolve(first), PhysicsErrors::HandleStale);
            RequireError(registry.Remove(first), PhysicsErrors::HandleStale);

            const auto replacement = registry.Acquire({18}).Value();
            REQUIRE(replacement.slot.index == first.slot.index);
            REQUIRE(replacement.slot.generation == first.slot.generation + 1);
            REQUIRE(registry.Resolve(replacement).Value()->value == 18);
            RequireError(registry.Resolve(first), PhysicsErrors::HandleStale);
        }

        TEST_CASE("Physics registries distinguish live capacity from permanent generation exhaustion", "[physics][registry]") {
            auto registry = Registry<ConstraintHandle, NativeConstraintId>::Create({.maximumSlots = 1, .maximumGeneration = 2}).Value();
            REQUIRE(registry.BindOwner(World(81)).HasValue());
            const auto first = registry.Acquire({1}).Value();
            RequireError(registry.Acquire({2}), PhysicsErrors::CapacityExceeded);
            REQUIRE(registry.Remove(first).HasValue());
            const auto last = registry.Acquire({3}).Value();
            REQUIRE(last.slot.generation == 2);
            REQUIRE(registry.Remove(last).HasValue());
            REQUIRE(registry.ActiveCount() == 0);
            RequireError(registry.Resolve(last), PhysicsErrors::HandleStale);
            RequireError(registry.Acquire({4}), PhysicsErrors::GenerationExhausted);
        }

        TEST_CASE("Physics registry preparation validates bounds including an admitted empty world", "[physics][registry]") {
            auto empty = Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 0}).Value();
            REQUIRE(empty.Capacity() == 0);
            REQUIRE(empty.BindOwner(World(91)).HasValue());
            RequireError(empty.Acquire({1}), PhysicsErrors::CapacityExceeded);

            RequireError(Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = MaximumPhysicsResourceRecords + 1}),
                         PhysicsErrors::CapacityExceeded);
            RequireError(Registry<BodyHandle, NativeBodyId>::Create({.maximumSlots = 1, .maximumGeneration = 0}),
                         PhysicsErrors::DescriptorInvalid);
        }
    }  // namespace
}  // namespace Horo::Physics
