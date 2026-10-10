#include "Horo/Runtime/Render/FramePacing.h"
#include "Horo/Runtime/Render/FramePacingErrors.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::Render::PacingTests {
    RenderSurfaceSnapshot Ready(const std::uint64_t revision = 1, const std::uint64_t generation = 1) {
        return {{1, generation},
                revision,
                RenderSurfaceState::Ready,
                RenderSurfaceConfiguration{{1280, 720}, {PresentMode::Fifo, PresentMode::Fifo, PresentModeResolution::Exact, 0}, revision},
                std::nullopt,
                std::nullopt};
    }

    Duration Ns(const std::int64_t value) {
        return Duration::FromNanoseconds(value);
    }

    template <typename T> bool HasCode(const Result<T> &result, const ErrorCodeDescriptor &code) {
        return result.HasError() && result.ErrorValue().code.Value() == code.code.Value() &&
               result.ErrorValue().domain.Value() == code.domain.Value();
    }

    TEST_CASE("Host cap yields bounded waits, ceiling periods and no catch-up bursts", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({60}, Ready()).HasValue());
        CHECK(pacer.Poll(Ns(0), false).Value().disposition == FramePacingDisposition::Ready);
        auto wait = pacer.Poll(Ns(1), false).Value();
        CHECK(wait.disposition == FramePacingDisposition::Wait);
        CHECK(wait.wait == Ns(2'000'000));
        CHECK(pacer.Poll(Ns(16'666'666), false).Value().wait == Ns(1));
        CHECK(pacer.Poll(Ns(16'666'667), false).Value().disposition == FramePacingDisposition::Ready);
        CHECK(pacer.Poll(Ns(100'000'000), false).Value().disposition == FramePacingDisposition::Ready);
        CHECK(pacer.Statistics().Value().missedDeadlines == 1);
        CHECK(pacer.Poll(Ns(100'000'000), false).Value().disposition == FramePacingDisposition::Wait);
    }

    TEST_CASE("Unlimited cap preserves cancellation and rejects malformed clocks", "[renderer][pacing]") {
        FramePacer pacer;
        CHECK(HasCode(pacer.Poll(Ns(0), false), FramePacingErrors::InvalidSurface));
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        CHECK(pacer.Poll(Ns(0), false).Value().disposition == FramePacingDisposition::Ready);
        CHECK(HasCode(pacer.Poll(Ns(0), true), FramePacingErrors::Cancelled));
        CHECK(HasCode(pacer.Poll(Ns(-1), false), FramePacingErrors::InvalidClock));
        CHECK(HasCode(pacer.Poll(Ns(std::numeric_limits<std::int64_t>::max()), false), FramePacingErrors::InvalidClock));
        REQUIRE(pacer.Poll(Ns(100), false).HasValue());
        CHECK(HasCode(pacer.Poll(Ns(99), false), FramePacingErrors::InvalidClock));
        REQUIRE(pacer.Reset().HasValue());
        CHECK(pacer.Poll(Ns(0), false).HasValue());
    }

    TEST_CASE("Host startup cancellation revokes a pending cap wait without advancing its clock", "[renderer][pacing][cancellation]") {
        class HostClock final : public Clock {
        public:
            Duration now;

            Duration MonotonicNow() const override {
                return now;
            }
        } clock;

        CancellationSource source;
        const CancellationToken hostToken = source.Token();
        FramePacer pacer;
        REQUIRE(pacer.Configure({1}, Ready()).HasValue());
        REQUIRE(pacer.Poll(clock.MonotonicNow(), hostToken).HasValue());
        REQUIRE(pacer.Poll(clock.MonotonicNow(), hostToken).Value().disposition == FramePacingDisposition::Wait);
        source.RequestCancellation();
        CHECK(HasCode(pacer.Poll(clock.MonotonicNow(), hostToken), FramePacingErrors::Cancelled));
        CHECK(clock.MonotonicNow() == Ns(0));
        CHECK(pacer.Statistics().Value().missedDeadlines == 0);
    }

    TEST_CASE("Invalid policy and stale surface publication cannot reset a live deadline", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({1}, Ready(2)).HasValue());
        REQUIRE(pacer.Poll(Ns(0), false).HasValue());
        CHECK(HasCode(pacer.Configure({1001}, Ready(3)), FramePacingErrors::InvalidPolicy));
        CHECK(HasCode(pacer.Configure({}, Ready()), FramePacingErrors::StaleEvidence));
        auto contradictory = Ready(2);
        contradictory.active->extent.width = 1;
        CHECK(HasCode(pacer.Configure({}, contradictory), FramePacingErrors::StaleEvidence));
        auto malformed = Ready(3);
        malformed.active.reset();
        CHECK(HasCode(pacer.Configure({}, malformed), FramePacingErrors::InvalidSurface));
        CHECK(pacer.Poll(Ns(999'999'999), false).Value().wait == Ns(1));
        CHECK(pacer.Poll(Ns(1'000'000'000), false).Value().disposition == FramePacingDisposition::Ready);
        REQUIRE(pacer.Configure({1000}, Ready(3)).HasValue());
        CHECK(pacer.Poll(Ns(1'000'000'000), false).Value().disposition == FramePacingDisposition::Ready);
        CHECK(pacer.Poll(Ns(1'000'999'999), false).Value().wait == Ns(1));
    }

    TEST_CASE("CPU present return and failures never fabricate native refresh", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        for (std::uint64_t frame = 1; frame <= 10; ++frame)
            REQUIRE(pacer
                        .RecordPresent(frame, Ns(static_cast<std::int64_t>(frame) * 20'000'000),
                                       Ns(static_cast<std::int64_t>(frame) * 20'000'000 + 4'000'000), frame != 5)
                        .HasValue());
        const auto stats = pacer.Statistics().Value();
        CHECK(stats.presentedFrames == 9);
        CHECK(stats.failedPresents == 1);
        CHECK(stats.lastPresentCallDuration == Ns(4'000'000));
        CHECK_FALSE(stats.estimatedRefreshHertz.has_value());
        CHECK(stats.nativeIntervalSamples == 0);
        CHECK(HasCode(pacer.RecordPresent(10, Ns(0), Ns(0), true), FramePacingErrors::StaleEvidence));
        CHECK(HasCode(pacer.RecordPresent(11, Ns(2), Ns(1), true), FramePacingErrors::InvalidClock));
    }

    TEST_CASE("Native refresh estimator uses refresh ordinals across dropped frames with bounded storage", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            const auto stamp = Ns(static_cast<std::int64_t>(frame) * 40'000'000);
            REQUIRE(
                pacer.RecordPresent(frame, stamp, stamp + Ns(1), true, NativePresentTiming{{1, 1}, frame, frame * 2, stamp}).HasValue());
            if (frame < 5)
                CHECK_FALSE(pacer.Statistics().Value().estimatedRefreshHertz.has_value());
        }
        const auto stats = pacer.Statistics().Value();
        REQUIRE(stats.estimatedRefreshHertz.has_value());
        CHECK(*stats.estimatedRefreshHertz == Catch::Approx(50));
        CHECK(stats.nativeIntervalSamples == 8);
        REQUIRE(pacer.Configure({}, Ready(2, 2)).HasValue());
        CHECK_FALSE(pacer.Statistics().Value().estimatedRefreshHertz.has_value());
        CHECK(pacer.Statistics().Value().nativeIntervalSamples == 0);
        CHECK(HasCode(pacer.RecordPresent(21, Ns(900'000'000), Ns(900'000'001), true, NativePresentTiming{{1, 1}, 21, 42, Ns(900'000'000)}),
                      FramePacingErrors::InvalidNativeTiming));
        CHECK(pacer.Statistics().Value().presentedFrames == 20);
    }

    TEST_CASE("Metal-style display feedback is native timing without claiming a refresh ordinal", "[renderer][pacing][native]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        for (std::uint64_t frame = 1; frame <= 8; ++frame) {
            const auto stamp = Ns(static_cast<std::int64_t>(frame) * 20'000'000);
            REQUIRE(pacer.RecordPresent(frame, stamp, stamp + Ns(1), true, NativePresentTiming{{1, 1}, frame, 0, stamp}).HasValue());
        }
        const auto stats = pacer.Statistics().Value();
        REQUIRE(stats.lastNativeDisplayTime.has_value());
        CHECK(*stats.lastNativeDisplayTime == Ns(160'000'000));
        CHECK_FALSE(stats.estimatedRefreshHertz.has_value());
        CHECK(stats.nativeIntervalSamples == 0);
    }

    TEST_CASE("Invalid native evidence is transactional and delayed observations remain distinct", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        const NativePresentTiming first{{1, 1}, 1, 1, Ns(10'000'000)};
        REQUIRE(pacer.RecordPresent(2, Ns(20'000'000), Ns(20'000'001), true, first).HasValue());
        CHECK(HasCode(pacer.RecordPresent(3, Ns(30'000'000), Ns(30'000'001), false, first), FramePacingErrors::InvalidNativeTiming));
        CHECK(HasCode(pacer.RecordPresent(3, Ns(30'000'000), Ns(30'000'001), true, first), FramePacingErrors::StaleEvidence));
        auto tooFast = NativePresentTiming{{1, 1}, 2, 2, Ns(10'000'001)};
        CHECK(HasCode(pacer.RecordPresent(3, Ns(30'000'000), Ns(30'000'001), true, tooFast), FramePacingErrors::InvalidNativeTiming));
        auto future = NativePresentTiming{{1, 1}, 2, 2, Ns(40'000'000)};
        CHECK(HasCode(pacer.RecordPresent(3, Ns(30'000'000), Ns(30'000'001), true, future), FramePacingErrors::InvalidNativeTiming));
        CHECK(pacer.Statistics().Value().presentedFrames == 1);
        REQUIRE(pacer.RecordPresent(3, Ns(30'000'000), Ns(30'000'001), true, NativePresentTiming{{1, 1}, 2, 2, Ns(30'000'000)}).HasValue());
    }

    TEST_CASE("Surface suspension, resize, focus reset and shutdown discard pacing baselines", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({60}, Ready()).HasValue());
        REQUIRE(pacer.Poll(Ns(0), false).HasValue());
        auto suspended = Ready(2);
        suspended.state = RenderSurfaceState::Suspended;
        REQUIRE(pacer.Configure({60}, suspended).HasValue());
        CHECK(pacer.Poll(Ns(1), false).Value().disposition == FramePacingDisposition::Suspended);
        CHECK(HasCode(pacer.RecordPresent(1, Ns(0), Ns(1), true), FramePacingErrors::InvalidSurface));
        REQUIRE(pacer.Configure({60}, Ready(3)).HasValue());
        CHECK(pacer.Poll(Ns(2), false).Value().disposition == FramePacingDisposition::Ready);
        REQUIRE(pacer.Reset().HasValue());
        CHECK(pacer.Poll(Ns(3), false).Value().disposition == FramePacingDisposition::Ready);
        REQUIRE(pacer.Stop().HasValue());
        REQUIRE(pacer.Stop().HasValue());
        CHECK(HasCode(pacer.Poll(Ns(4), false), FramePacingErrors::Stopped));
        CHECK(HasCode(pacer.Configure({}, Ready(4)), FramePacingErrors::Stopped));
        CHECK(HasCode(pacer.Reset(), FramePacingErrors::Stopped));
    }

    TEST_CASE("Pacing mutations, observations and diagnostics enforce the host owner thread", "[renderer][pacing]") {
        FramePacer pacer;
        REQUIRE(pacer.Configure({}, Ready()).HasValue());
        bool rejected = false;
        std::thread worker([&] {
            rejected = HasCode(pacer.Configure({}, Ready()), FramePacingErrors::WrongThread) &&
                       HasCode(pacer.Poll(Ns(0), false), FramePacingErrors::WrongThread) &&
                       HasCode(pacer.RecordPresent(1, Ns(0), Ns(1), true), FramePacingErrors::WrongThread) &&
                       HasCode(pacer.Statistics(), FramePacingErrors::WrongThread) &&
                       HasCode(pacer.Reset(), FramePacingErrors::WrongThread) && HasCode(pacer.Stop(), FramePacingErrors::WrongThread);
        });
        worker.join();
        CHECK(rejected);
        CHECK(pacer.Poll(Ns(0), false).HasValue());
    }
}  // namespace Horo::Render::PacingTests
