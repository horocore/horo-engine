#include "UiAnimationPlayback.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::AnimationInternal;

    UiAnimationTimePolicy Policy(const std::int64_t duration) {
        UiAnimationTimePolicy policy;
        policy.duration = {duration};
        return policy;
    }

    PlaybackCursor Advance(const PlaybackCursor &cursor, const UiAnimationTimePolicy &policy, const std::int64_t delta,
                           const std::uint32_t budget = 8) {
        auto result = AdvancePlayback(cursor, policy, {delta}, budget);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

TEST_CASE("UI playback integer progress has exact endpoints and independently checked large ratios", "[runtime_ui][animation]") {
    constexpr auto Maximum = std::numeric_limits<std::uint32_t>::max();
    CHECK(ClosedProgress(0, 13) == 0);
    CHECK(ClosedProgress(13, 13) == Maximum);
    CHECK(ClosedProgress(1, 2) == 2'147'483'647U);
    CHECK(ClosedProgress(1, 3) == 1'431'655'765U);
    constexpr auto SignedMaximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    CHECK(ClosedProgress(SignedMaximum - 1, SignedMaximum) == Maximum - 1);
}

TEST_CASE("UI playback carries rational remainder identically across equivalent sample histories", "[runtime_ui][animation]") {
    auto policy = Policy(100);
    policy.rate = {1, 3};
    const auto one = Advance({}, policy, 1);
    CHECK(one.sample.elapsed.nanoseconds == 0);
    CHECK(one.remainder == UiTimeRemainder{1, 3});
    const auto two = Advance(one, policy, 1);
    CHECK(two.remainder == UiTimeRemainder{2, 3});
    const auto three = Advance(two, policy, 1);
    const auto whole = Advance({}, policy, 3);
    CHECK(three.sample.elapsed == whole.sample.elapsed);
    CHECK(three.sample.progress == whole.sample.progress);
    CHECK(three.remainder == whole.remainder);
    policy.rate = {2, 3};
    const auto changed = Advance(one, policy, 1);
    CHECK(changed.sample.elapsed.nanoseconds == 1);
    CHECK(changed.remainder == UiTimeRemainder{});
}

TEST_CASE("UI playback delay fill and finite alternating completion are explicit", "[runtime_ui][animation]") {
    auto policy = Policy(10);
    policy.delay = {5};
    policy.loop.iterations = 2;
    policy.direction = UiPlaybackDirection::Alternating;
    policy.fill = UiAnimationFill::Both;
    const auto waiting = Advance({}, policy, 4);
    CHECK(waiting.sample.state == UiAnimationState::Waiting);
    CHECK(waiting.sample.contributesValue);
    const auto first = Advance(waiting, policy, 6);
    CHECK(first.sample.state == UiAnimationState::Running);
    CHECK(first.sample.iteration == 0);
    CHECK(first.sample.progress == 2'147'483'647U);
    const auto reversed = Advance(first, policy, 10);
    CHECK(reversed.sample.iteration == 1);
    CHECK(reversed.sample.progress == 2'147'483'648U);
    const auto completed = Advance(reversed, policy, 5);
    CHECK(completed.sample.elapsed.nanoseconds == 25);
    CHECK(completed.sample.state == UiAnimationState::Completed);
    CHECK(completed.sample.progress == 0);
    CHECK(completed.sample.newTerminalOutcome);
    const auto repeated = Advance(completed, policy, 100);
    CHECK_FALSE(repeated.sample.newTerminalOutcome);
    CHECK(repeated.sample.elapsed == completed.sample.elapsed);
}

TEST_CASE("UI zero duration resolves once in the same candidate without hidden delay or division", "[runtime_ui][animation]") {
    auto policy = Policy(0);
    const auto completed = Advance({}, policy, 0, 1);
    CHECK(completed.sample.outcome == UiAnimationOutcome::Completed);
    CHECK(completed.sample.progress == std::numeric_limits<std::uint32_t>::max());
    CHECK(completed.sample.newTerminalOutcome);
    CHECK(completed.sample.crossedIterations == 1);
    CHECK_FALSE(Advance(completed, policy, 0, 0).sample.newTerminalOutcome);
    CHECK(AdvancePlayback({}, policy, {}, 0).HasError());
    policy.direction = UiPlaybackDirection::Reverse;
    CHECK(Advance({}, policy, 0).sample.progress == 0);
    policy.loop.iterations = 2;
    CHECK(ValidatePlaybackPolicy(policy).HasError());
}

TEST_CASE("UI iteration budget rejects complete work rather than partially advancing", "[runtime_ui][animation]") {
    auto policy = Policy(2);
    policy.loop.iterations = 10;
    const PlaybackCursor current;
    auto tooMany = AdvancePlayback(current, policy, {20}, 9);
    REQUIRE(tooMany.HasError());
    CHECK(tooMany.ErrorValue().code.Value() == UiErrors::AnimationBudgetExceeded.code.Value());
    CHECK(current.sample.elapsed.nanoseconds == 0);
    CHECK(current.sample.outcome == UiAnimationOutcome::None);
    const auto completed = Advance(current, policy, 20, 10);
    CHECK(completed.sample.crossedIterations == 10);
    CHECK(completed.sample.newTerminalOutcome);
}

TEST_CASE("UI hold is explicit and infinite motion cannot become a required lifecycle blocker", "[runtime_ui][animation]") {
    auto policy = Policy(10);
    policy.rate = {0, 1};
    CHECK(ValidatePlaybackPolicy(policy).HasError());
    policy.zeroRate = UiZeroRatePolicy::Hold;
    const auto held = Advance({}, policy, 100);
    CHECK(held.sample.state == UiAnimationState::Held);
    CHECK(held.sample.elapsed.nanoseconds == 0);
    policy.rate = {1, 1};
    policy.loop.kind = UiLoopKind::Infinite;
    CHECK(ValidatePlaybackPolicy(policy).HasValue());
    policy.lifecycle = UiAnimationLifecycle::RequiredEnter;
    CHECK(ValidatePlaybackPolicy(policy).HasError());
    policy.lifecycle = UiAnimationLifecycle::NonBlocking;
    policy.domain = UiTimeDomain::Simulation;
    CHECK(ValidatePlaybackPolicy(policy).HasError());
}

TEST_CASE("UI playback rejects malformed policy and checked numeric overflow transactionally", "[runtime_ui][animation]") {
    auto policy = Policy(std::numeric_limits<std::int64_t>::max());
    policy.delay = {1};
    CHECK(ValidatePlaybackPolicy(policy).HasError());
    policy = Policy(1);
    policy.direction = static_cast<UiPlaybackDirection>(255);
    CHECK(ValidatePlaybackPolicy(policy).HasError());
    policy = Policy(100);
    CHECK(AdvancePlayback({}, policy, {-1}, 8).HasError());
    policy.rate = {2, 1};
    auto overflow = AdvancePlayback({}, policy, {std::numeric_limits<std::int64_t>::max()}, 8);
    REQUIRE(overflow.HasError());
    CHECK(overflow.ErrorValue().code.Value() == UiErrors::ClockOverflow.code.Value());
    policy.rate.denominator = 0;
    CHECK(ValidatePlaybackPolicy(policy).HasError());
}

TEST_CASE("UI completion and cancellation are mutually exclusive exactly-once candidates", "[runtime_ui][animation]") {
    auto cancelled = CancelPlayback({}, UiAnimationCancellation::Shutdown);
    REQUIRE(cancelled.HasValue());
    CHECK(cancelled.Value().sample.outcome == UiAnimationOutcome::Cancelled);
    CHECK(cancelled.Value().sample.newTerminalOutcome);
    CHECK_FALSE(cancelled.Value().sample.contributesValue);
    auto repeated = CancelPlayback(cancelled.Value(), UiAnimationCancellation::Shutdown);
    REQUIRE(repeated.HasValue());
    CHECK_FALSE(repeated.Value().sample.newTerminalOutcome);
    CHECK(CancelPlayback(cancelled.Value(), UiAnimationCancellation::Reload).HasError());
    const auto policy = Policy(1);
    CHECK_FALSE(Advance(cancelled.Value(), policy, 10).sample.newTerminalOutcome);
    const auto completed = Advance({}, policy, 1);
    CHECK(CancelPlayback(completed, UiAnimationCancellation::Explicit).HasError());
    CHECK(CancelPlayback({}, UiAnimationCancellation::None).HasError());
}
