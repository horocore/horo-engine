#include "runtime/renderer/modules/metal/MetalPresentationFeedback.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <thread>

namespace Horo::Render::Detail::FeedbackTests {
    NativePresentTiming Timing(std::uint64_t frame, std::int64_t nanoseconds = 10) {
        return {{1, 1}, frame, 0, Duration::FromNanoseconds(nanoseconds), {}};
    }

    TEST_CASE("Metal feedback coalesces bounded values and rejects stale order and generation", "[renderer][metal][pacing]") {
        MetalPresentationFeedback feedback;
        REQUIRE(feedback.SelectSurface({1, 1}));
        feedback.Publish(Timing(1));
        feedback.Publish(Timing(2, 20));
        feedback.Publish(Timing(1));
        feedback.Publish(Timing(3, 0));
        auto sample = feedback.Poll();
        REQUIRE(sample.has_value());
        CHECK(sample->frameNumber == 2);
        CHECK(feedback.DroppedCount() == 3);
        CHECK_FALSE(feedback.Poll().has_value());
        feedback.Publish(Timing(3, 19));
        CHECK_FALSE(feedback.Poll().has_value());
        REQUIRE(feedback.SelectSurface({1, 2}));
        feedback.Publish(Timing(4, 40));
        CHECK_FALSE(feedback.Poll().has_value());
        auto current = Timing(5, 50);
        current.surface.generation = 2;
        feedback.Publish(current);
        REQUIRE(feedback.Poll().has_value());
    }

    TEST_CASE("Late Metal callbacks retain only closed feedback state after host destruction", "[renderer][metal][pacing]") {
        auto owner = std::make_shared<MetalPresentationFeedback>();
        REQUIRE(owner->SelectSurface({1, 1}));
        auto callback = [state = owner] {
            state->Publish(Timing(1));
        };
        std::weak_ptr<MetalPresentationFeedback> weak = owner;
        owner->Close();
        owner.reset();
        std::thread native([callback] {
            callback();
        });
        native.join();
        auto retained = weak.lock();
        REQUIRE(retained != nullptr);
        CHECK_FALSE(retained->Poll().has_value());
        CHECK(retained->DroppedCount() == 1);
        CHECK_FALSE(retained->SelectSurface({1, 2}));
    }

    TEST_CASE("Metal host clock calibration excludes dropped timestamps and never invents refresh ordinals", "[renderer][metal][pacing]") {
        const MetalPresentationCalibration conversion{10.0, Duration::FromNanoseconds(2'000'000'000),
                                                      Duration::FromNanoseconds(2'000'000'020)};
        CHECK_FALSE(conversion.Translate({1, 1}, 1, 0.0).has_value());
        const auto timing = conversion.Translate({1, 1}, 1, 10.02);
        REQUIRE(timing.has_value());
        CHECK(timing->displayTime.ToNanoseconds() >= 2'020'000'009);
        CHECK(timing->displayTime.ToNanoseconds() <= 2'020'000'011);
        CHECK(timing->clockUncertainty.ToNanoseconds() == 11);
        CHECK(timing->displayOrdinal == 0);
        auto invalid = conversion;
        invalid.after = Duration::FromMilliseconds(2002);
        CHECK_FALSE(invalid.Translate({1, 1}, 1, 10.02).has_value());
        invalid.after = Duration::FromNanoseconds(1);
        CHECK_FALSE(invalid.Translate({1, 1}, 1, 10.02).has_value());
    }
}  // namespace Horo::Render::Detail::FeedbackTests
