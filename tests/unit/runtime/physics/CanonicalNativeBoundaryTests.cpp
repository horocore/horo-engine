#include "canonical/solver/CanonicalSolver.h"

#include <Jolt/Jolt.h>

// Jolt's subsidiary headers require its root header to be processed first.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Canonical solver ABI inspection leaves native registration and allocator state untouched", "[physics][native][composition]") {
    REQUIRE(JPH::Factory::sInstance == nullptr);
    const auto allocate = JPH::Allocate;
    const auto reallocate = JPH::Reallocate;
    const auto free = JPH::Free;
    const auto alignedAllocate = JPH::AlignedAllocate;
    const auto alignedFree = JPH::AlignedFree;
    REQUIRE(Horo::Physics::Detail::IsCanonicalSolverBuildCompatible());
    REQUIRE_FALSE(JPH::VerifyJoltVersionIDInternal(0));
    REQUIRE(JPH::Factory::sInstance == nullptr);
    REQUIRE(JPH::Allocate == allocate);
    REQUIRE(JPH::Reallocate == reallocate);
    REQUIRE(JPH::Free == free);
    REQUIRE(JPH::AlignedAllocate == alignedAllocate);
    REQUIRE(JPH::AlignedFree == alignedFree);
}
