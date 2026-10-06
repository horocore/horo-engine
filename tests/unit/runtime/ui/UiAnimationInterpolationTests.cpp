#include "UiAnimationInterpolation.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::AnimationInternal;
}  // namespace

TEST_CASE("UI dimension interpolation preserves signed endpoints and full-width bounded arithmetic", "[runtime_ui][animation]") {
    constexpr auto Low = std::numeric_limits<std::int32_t>::min();
    constexpr auto High = std::numeric_limits<std::int32_t>::max();
    constexpr auto End = std::numeric_limits<std::uint32_t>::max();
    CHECK(InterpolateUnits(Low, High, {0}) == Low);
    CHECK(InterpolateUnits(Low, High, {End}) == High);
    CHECK(InterpolateUnits(Low, High, {2'147'483'648U}) == 0);
    CHECK(InterpolateUnits(High, Low, {2'147'483'648U}) == -1);
    CHECK(InterpolateUnits(-17, 19, {End}) == 19);
    CHECK(InterpolateUnits(19, -17, {End}) == -17);
}

TEST_CASE("UI color interpolation preserves semantic role and rejects category changes", "[runtime_ui][animation]") {
    const UiStyleValue from = UiStyleColor{0, 0, 0, 1, UiStyleColorRole::Accent};
    const UiStyleValue to = UiStyleColor{1, 1, 1, 0, UiStyleColorRole::Accent};
    auto first = InterpolateValue(from, to, {0});
    auto last = InterpolateValue(from, to, {std::numeric_limits<std::uint32_t>::max()});
    REQUIRE(first.HasValue());
    REQUIRE(last.HasValue());
    CHECK(first.Value() == from);
    CHECK(last.Value() == to);
    const UiStyleValue foreignRole = UiStyleColor{1, 1, 1, 0, UiStyleColorRole::Text};
    CHECK(InterpolateValue(from, foreignRole, {0}).HasError());
    CHECK(InterpolateValue(from, UiStyleDimension{1}, {0}).HasError());
    CHECK(InterpolateValue(UiStyleEnumValue{0}, UiStyleEnumValue{1}, {0}).HasError());
}

TEST_CASE("UI scalar and shape interpolation reject malformed endpoints", "[runtime_ui][animation]") {
    const UiStyleValue valid = UiStyleScalar{1};
    const UiStyleValue malformed = UiStyleScalar{std::numeric_limits<float>::infinity()};
    CHECK(InterpolateValue(valid, malformed, {0}).HasError());
    CHECK(InterpolateValue(malformed, valid, {std::numeric_limits<std::uint32_t>::max()}).HasError());
    const UiStyleValue shape = UiStyleShape{10, 2, 1, -4, 4, 3};
    const UiStyleValue invalidShape = UiStyleShape{-1, 2, 1, 0, 0, 0};
    CHECK(InterpolateValue(shape, invalidShape, {0}).HasError());
    auto retained = InterpolateValue(shape, shape, {2'147'483'647U});
    REQUIRE(retained.HasValue());
    CHECK(retained.Value() == shape);
}
