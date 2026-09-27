#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationTestAssertions.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    TEST_CASE("Omitted Detour crowd provider reports unsupported without publishing a backend", "[unit][navigation][crowd][provider]") {
        TestSupport::RequireError(CreateRecastDetourCrowdBackend(), NavigationErrors::OperationUnsupported);
    }
}  // namespace Horo::Navigation
