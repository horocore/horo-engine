#include "UiAnimationIncarnations.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Actual child-clock reservation burns all admitted incarnations and never wraps at exhaustion",
          "[runtime_ui][animation][identity]") {
    using Horo::Runtime::Ui::AnimationInternal::ReserveChildIncarnations;
    std::uint32_t issued = 1;
    REQUIRE(ReserveChildIncarnations(issued, 2) == 2);
    REQUIRE(issued == 3);
    // Cancelling a gate never rolls its issuing ledger back; the next real admission uses this same primitive.
    REQUIRE(ReserveChildIncarnations(issued, 1) == 4);
    REQUIRE(issued == 4);
    issued = std::numeric_limits<std::uint32_t>::max() - 1;
    REQUIRE_FALSE(ReserveChildIncarnations(issued, 2));
    REQUIRE(issued == std::numeric_limits<std::uint32_t>::max() - 1);
    REQUIRE(ReserveChildIncarnations(issued, 1) == std::numeric_limits<std::uint32_t>::max());
    REQUIRE(issued == std::numeric_limits<std::uint32_t>::max());
    REQUIRE_FALSE(ReserveChildIncarnations(issued, 1));
    REQUIRE(issued == std::numeric_limits<std::uint32_t>::max());
    REQUIRE(ReserveChildIncarnations(issued, 0) == issued);
}
