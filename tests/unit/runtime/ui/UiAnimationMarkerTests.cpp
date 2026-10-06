#include "UiAnimationMarkers.h"
#include "UiTestUtils.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::AnimationInternal;
    using Test::Stable;

    UiAnimationDefinition Definition(const std::int64_t duration) {
        UiAnimationDefinition definition;
        definition.time.duration = {duration};
        definition.markers = {{Stable<UiAnimationMarkerId>(1), 0},
                              {Stable<UiAnimationMarkerId>(2), std::numeric_limits<std::uint32_t>::max()}};
        return definition;
    }

    MarkerPublication Publication(const std::uint32_t capacity = 16) {
        return {{UiOwnershipGeneration::Create(93).Value(), 4, 1}, 1, capacity};
    }
}  // namespace

TEST_CASE("UI marker crossings retain exact iteration boundary order and never repeat extraction events", "[runtime_ui][animation]") {
    auto definition = Definition(10);
    definition.time.loop.iterations = 2;
    definition.time.direction = UiPlaybackDirection::Alternating;
    std::vector<UiAnimationMarkerCrossing> crossings;
    crossings.reserve(16);
    auto initial = AdvancePlayback({}, definition.time, {}, 8);
    REQUIRE(initial.HasValue());
    REQUIRE(AppendMarkers(definition, Publication(), {}, initial.Value(), true, crossings).HasValue());
    REQUIRE(crossings.size() == 1);
    CHECK(crossings[0].marker == Stable<UiAnimationMarkerId>(1));
    crossings.clear();
    auto completed = AdvancePlayback(initial.Value(), definition.time, {20}, 8);
    REQUIRE(completed.HasValue());
    REQUIRE(AppendMarkers(definition, Publication(), initial.Value(), completed.Value(), false, crossings).HasValue());
    REQUIRE(crossings.size() == 3);
    CHECK(crossings[0].marker == Stable<UiAnimationMarkerId>(2));
    CHECK(crossings[0].iteration == 0);
    CHECK_FALSE(crossings[0].reverse);
    CHECK(crossings[1].marker == Stable<UiAnimationMarkerId>(2));
    CHECK(crossings[1].iteration == 1);
    CHECK(crossings[1].reverse);
    CHECK(crossings[2].marker == Stable<UiAnimationMarkerId>(1));
    const auto size = crossings.size();
    REQUIRE(AppendMarkers(definition, Publication(), completed.Value(), completed.Value(), false, crossings).HasValue());
    CHECK(crossings.size() == size);
}

TEST_CASE("UI marker arithmetic preserves large exact endpoints and zero-duration terminal crossings", "[runtime_ui][animation]") {
    constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto End = std::numeric_limits<std::uint32_t>::max();
    CHECK(MarkerOffset(Maximum, 0) == 0);
    CHECK(MarkerOffset(Maximum, End) == Maximum);
    CHECK(MarkerOffset(10, End / 2) == 5);
    auto definition = Definition(0);
    auto completed = AdvancePlayback({}, definition.time, {}, 8);
    REQUIRE(completed.HasValue());
    std::vector<UiAnimationMarkerCrossing> crossings;
    crossings.reserve(4);
    REQUIRE(AppendMarkers(definition, Publication(4), {}, completed.Value(), true, crossings).HasValue());
    REQUIRE(crossings.size() == 2);
    REQUIRE(AppendMarkers(definition, Publication(4), completed.Value(), completed.Value(), false, crossings).HasValue());
    CHECK(crossings.size() == 2);
}

TEST_CASE("UI marker work capacity rejects the inactive candidate without changing playback cursors", "[runtime_ui][animation]") {
    const auto definition = Definition(10);
    const auto initial = AdvancePlayback({}, definition.time, {}, 8).Value();
    const auto completed = AdvancePlayback(initial, definition.time, {10}, 8).Value();
    std::vector<UiAnimationMarkerCrossing> crossings;
    const auto rejected = AppendMarkers(definition, Publication(0), initial, completed, false, crossings);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiErrors::AnimationBudgetExceeded.code.Value());
    CHECK(crossings.empty());
    CHECK(initial.sample.outcome == UiAnimationOutcome::None);
    CHECK(completed.sample.outcome == UiAnimationOutcome::Completed);
}
