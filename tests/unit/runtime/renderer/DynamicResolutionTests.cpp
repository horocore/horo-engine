#include "Horo/Runtime/Render/DynamicResolution.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace std::chrono_literals;
    constexpr RenderViewId View{1};
    constexpr FramebufferExtent Output{1920, 1080};
    constexpr DynamicResolutionPolicy Policy{.raiseBelow = 10ms,
                                             .lowerAbove = 16ms,
                                             .minimumPermille = 500,
                                             .maximumPermille = 1000,
                                             .stepPermille = 100,
                                             .raiseAfter = 3,
                                             .lowerAfter = 2,
                                             .settlingSamples = 2};

    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "accepted"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &error) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == error.code.Value());
    }
}  // namespace

TEST_CASE("Dynamic resolution publishes only the exact prepared extent", "[renderer][dynamic-resolution]") {
    DynamicResolutionController controller;
    REQUIRE(controller.Configure(View, Policy, Output, 900).HasValue());
    CHECK_FALSE(Take(controller.Observe({View, 1, 1, 18ms})));
    CHECK_FALSE(Take(controller.Observe({View, 2, 1, 16ms})));
    CHECK_FALSE(Take(controller.Observe({View, 3, 1, 18ms})));
    const auto change = Take(controller.Observe({View, 4, 1, 18ms}));
    REQUIRE(change);
    CHECK(change->reason == DynamicResolutionReason::AboveBudget);
    CHECK(change->candidate == DynamicResolutionState{View, Output, {1536, 864}, 2, 800});
    CHECK(Take(controller.Snapshot()).active.renderExtent == FramebufferExtent{1728, 972});
    CHECK_FALSE(Take(controller.Observe({View, 5, 1, 18ms})));
    auto forged = *change;
    forged.candidate.renderExtent.width += 1;
    ExpectError(controller.Resolve(forged, true), DynamicResolutionErrors::StaleChange);
    REQUIRE(controller.Resolve(*change, true).HasValue());
    const auto applied = Take(controller.Snapshot());
    CHECK(applied.active == change->candidate);
    CHECK(applied.publishedChanges == 1);
    CHECK_FALSE(applied.pending);
    CHECK_FALSE(applied.lastSample);
    ExpectError(controller.Resolve(*change, true), DynamicResolutionErrors::StaleChange);
    ExpectError(controller.Observe({View, 6, 1, 18ms}), DynamicResolutionErrors::StaleTiming);
}

TEST_CASE("Dynamic resolution settles after apply or rejection and requires consecutive evidence", "[renderer][dynamic-resolution]") {
    DynamicResolutionController controller;
    REQUIRE(controller.Configure(View, Policy, Output, 900).HasValue());
    CHECK_FALSE(Take(controller.Observe({View, 1, 1, 18ms})));
    const auto lower = Take(controller.Observe({View, 2, 1, 18ms}));
    REQUIRE(lower);
    REQUIRE(controller.Resolve(*lower, true).HasValue());
    for (std::uint64_t frame = 3; frame <= 6; ++frame)
        CHECK_FALSE(Take(controller.Observe({View, frame, 2, 8ms})));
    const auto raise = Take(controller.Observe({View, 7, 2, 8ms}));
    REQUIRE(raise);
    CHECK(raise->reason == DynamicResolutionReason::BelowBudget);
    CHECK(raise->candidate.scalePermille == 900);
    REQUIRE(controller.Resolve(*raise, false).HasValue());
    const auto rejected = Take(controller.Snapshot());
    CHECK(rejected.rejectedChanges == 1);
    CHECK(rejected.active.scalePermille == 800);
    CHECK(rejected.active.generation == 2);
    CHECK_FALSE(Take(controller.Observe({View, 8, 2, 18ms})));
    CHECK_FALSE(Take(controller.Observe({View, 9, 2, 18ms})));
    CHECK_FALSE(Take(controller.Observe({View, 10, 2, 18ms})));
    CHECK(Take(controller.Observe({View, 11, 2, 18ms})).has_value());
}

TEST_CASE("Dynamic resolution rejects stale GPU results and cancels superseded candidates", "[renderer][dynamic-resolution]") {
    DynamicResolutionController controller;
    REQUIRE(controller.Configure(View, Policy, Output, 1000).HasValue());
    ExpectError(controller.Observe({View, 1, 1, 0ns}), DynamicResolutionErrors::InvalidTiming);
    ExpectError(controller.Observe({{2}, 1, 1, 19ms}), DynamicResolutionErrors::StaleTiming);
    CHECK_FALSE(Take(controller.Observe({View, 1, 1, 19ms})));
    ExpectError(controller.Observe({View, 1, 1, 19ms}), DynamicResolutionErrors::StaleTiming);
    const auto pending = Take(controller.Observe({View, 2, 1, 19ms}));
    REQUIRE(pending);
    REQUIRE(controller.UpdateOutputExtent(Output).HasValue());
    CHECK(Take(controller.Snapshot()).pending == pending);
    REQUIRE(controller.UpdateOutputExtent({1921, 1081}).HasValue());
    const auto resized = Take(controller.Snapshot());
    CHECK(resized.active == DynamicResolutionState{View, {1921, 1081}, {1921, 1081}, 2, 1000});
    CHECK_FALSE(resized.pending);
    CHECK_FALSE(resized.lastSample);
    ExpectError(controller.Resolve(*pending, true), DynamicResolutionErrors::StaleChange);
    ExpectError(controller.Observe({View, 3, 1, 19ms}), DynamicResolutionErrors::StaleTiming);
}

TEST_CASE("Dynamic resolution clamps scale and handles quantized and maximum-size outputs", "[renderer][dynamic-resolution]") {
    auto policy = Policy;
    policy.stepPermille = 10;
    DynamicResolutionController controller;
    REQUIRE(controller.Configure(View, policy, {3, 3}, 1000).HasValue());
    CHECK_FALSE(Take(controller.Observe({View, 1, 1, 18ms})));
    const auto resize = Take(controller.Observe({View, 2, 1, 18ms}));
    REQUIRE(resize);
    CHECK(resize->candidate.renderExtent == FramebufferExtent{2, 2});
    CHECK(resize->candidate.scalePermille == 660);
    REQUIRE(controller.Resolve(*resize, true).HasValue());
    for (std::uint64_t frame = 3; frame <= 8; ++frame)
        CHECK_FALSE(Take(controller.Observe({View, frame, 2, 18ms})));
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(controller.UpdateOutputExtent({limit, 1}).HasValue());
    CHECK(Take(controller.Snapshot()).active.renderExtent == FramebufferExtent{2834678415U, 1});
}

TEST_CASE("Dynamic resolution owns configuration and rejects use after stop", "[renderer][dynamic-resolution]") {
    DynamicResolutionController controller;
    ExpectError(controller.Snapshot(), DynamicResolutionErrors::InvalidState);
    auto invalid = Policy;
    invalid.lowerAbove = invalid.raiseBelow;
    ExpectError(controller.Configure(View, invalid, Output, 900), DynamicResolutionErrors::InvalidConfiguration);
    ExpectError(controller.Configure(View, Policy, Output, 400), DynamicResolutionErrors::InvalidConfiguration);
    REQUIRE(controller.Configure(View, Policy, Output, 900).HasValue());
    auto foreign = Result<void>::Success();
    std::jthread other([&controller, &foreign] {
        foreign = controller.Stop();
    });
    other.join();
    ExpectError(foreign, DynamicResolutionErrors::WrongThread);
    REQUIRE(controller.Stop().HasValue());
    REQUIRE(controller.Stop().HasValue());
    ExpectError(controller.Observe({View, 1, 1, 19ms}), DynamicResolutionErrors::InvalidState);
}
