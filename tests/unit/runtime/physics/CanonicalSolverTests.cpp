#include "canonical/solver/CanonicalSolver.h"

#include <catch2/catch_test_macros.hpp>

#if __has_include(<Jolt/Jolt.h>) || defined(JPH_OBJECT_LAYER_BITS)
#error "Physics consumers must not inherit the native solver SDK or compile definitions"
#endif

namespace Horo::Physics {
    TEST_CASE("Canonical solver composition reports its actual linked build without activation", "[physics][composition]") {
        constexpr bool expectedNative = HORO_TEST_PHYSICS_NATIVE != 0;
        REQUIRE(Detail::IsCanonicalSolverBuildCompatible() == expectedNative);
    }
}  // namespace Horo::Physics
