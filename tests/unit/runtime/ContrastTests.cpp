#include "Horo/Accessibility/Contrast.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Accessibility {
    TEST_CASE("Contrast reports AA and AAA against actual sRGB without rounding borderline results", "[accessibility][contrast]") {
        constexpr SrgbColor white{1, 1, 1};
        constexpr SrgbColor black{};
        const auto maximum = EvaluateTextContrast(white, black).Value();
        REQUIRE(maximum.ratio == Catch::Approx(21));
        REQUIRE(maximum.meetsAA);
        REQUIRE(maximum.meetsAAA);
        const auto same = EvaluateTextContrast(white, white).Value();
        REQUIRE(same.ratio == Catch::Approx(1));
        REQUIRE_FALSE(same.meetsAA);
        constexpr double grey = 119.0 / 255.0;
        const auto borderline = EvaluateTextContrast({grey, grey, grey}, white).Value();
        REQUIRE(borderline.ratio == Catch::Approx(4.478089).margin(0.000001));
        REQUIRE_FALSE(borderline.meetsAA);
        REQUIRE_FALSE(borderline.meetsAAA);
        const auto large = EvaluateTextContrast({grey, grey, grey}, white, ContrastTextSize::Large).Value();
        REQUIRE(large.meetsAA);
        REQUIRE_FALSE(large.meetsAAA);
        REQUIRE(EvaluateTextContrast(white, {grey, grey, grey}).Value().ratio == borderline.ratio);
        REQUIRE(EvaluateTextContrast({std::numeric_limits<double>::quiet_NaN(), 0, 0}, white).HasError());
        REQUIRE(EvaluateTextContrast({-0.01, 0, 0}, white).HasError());
        REQUIRE(EvaluateTextContrast(white, {1.01, 0, 0}).HasError());
        REQUIRE(EvaluateTextContrast(white, black, static_cast<ContrastTextSize>(255)).HasError());
    }
}  // namespace Horo::Accessibility
