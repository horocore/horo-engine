#include "Horo/Cinematic/SequenceEvaluation.h"
#include "Horo/Cinematic/SequenceEvaluationErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string_view>

namespace Horo::Cinematic {
    namespace {
        [[nodiscard]] constexpr SequencePlayerHandle Handle(const std::uint64_t player = 7, const std::uint32_t generation = 1) {
            return {{41, 3}, {player, generation}};
        }

        [[nodiscard]] constexpr TrackId Track(const std::uint64_t value) {
            return {value, 1};
        }

        [[nodiscard]] constexpr KeyframeId Key(const std::uint64_t value) {
            return {value, 1};
        }

        [[nodiscard]] constexpr SequencePlayerSnapshot Playing(const SequenceTime position = 0, const SequencePlaybackRate rate = {1, 1},
                                                               const std::uint64_t controlRevision = 2) {
            return {Handle(), SequencePlaybackState::Playing, position, 10, rate, controlRevision};
        }

        struct AppliedValue final {
            TrackId track;
            float value{};
        };

        struct TrackProbe final {
            TrackId track;
            float offset{};
            std::array<AppliedValue, 8> *applied{};
            std::size_t *appliedCount{};
            bool failSample{};
        };

        [[nodiscard]] Result<float> SampleTrack(const void *context, const SequenceTime time) {
            const auto &probe = *static_cast<const TrackProbe *>(context);
            if (probe.failSample)
                return Result<float>::Failure(MakeError(SequenceEvaluationErrors::Malformed));
            return Result<float>::Success(static_cast<float>(time) + probe.offset);
        }

        void ApplyTrack(void *context, const float value) noexcept {
            auto &probe = *static_cast<TrackProbe *>(context);
            (*probe.applied)[(*probe.appliedCount)++] = {probe.track, value};
        }

        struct HookProbe final {
            std::array<SequenceFrameEventOccurrence, 16> events{};
            std::array<SequenceFrameCameraCutRequest, 16> cuts{};
            std::size_t eventCount{};
            std::size_t cutCount{};
        };

        Result<void> OnEvent(const BorrowedCallbackContext &context, const std::span<const SequenceFrameEventOccurrence> events) {
            auto &probe = *context.Get<HookProbe>();
            for (const SequenceFrameEventOccurrence &event : events)
                probe.events[probe.eventCount++] = event;
            return Result<void>::Success();
        }

        void OnCamera(void *context, const SequenceFrameCameraCutRequest &cut) noexcept {
            auto &probe = *static_cast<HookProbe *>(context);
            probe.cuts[probe.cutCount++] = cut;
        }

        [[nodiscard]] SequenceFrameHooks Hooks(HookProbe &probe) {
            return {BorrowedCallbackContext{&probe}, OnEvent, &probe, OnCamera};
        }

        struct ScratchStorage final {
            std::array<SequenceSampledValue, 8> values{};
            std::array<SequenceFrameEventOccurrence, 16> events{};
            std::array<SequenceFrameCameraCutRequest, 16> cuts{};

            [[nodiscard]] SequenceFrameScratch View() {
                return {values, events, cuts};
            }
        };

        [[nodiscard]] SequenceFrameEvaluationPlan Plan(std::span<const SequenceFrameTrackDescriptor> tracks = {},
                                                       std::span<const SequenceFrameEventKey> events = {},
                                                       std::span<const SequenceFrameCameraCutKey> cuts = {},
                                                       const SequenceLoopMode loop = SequenceLoopMode::Once,
                                                       const std::size_t crossings = 4) {
            auto plan = SequenceFrameEvaluationPlan::Create(10, loop, crossings, tracks, events, cuts);
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            const auto &actual = result.ErrorValue();
            CHECK((actual.domain.Value() == expected.domain.Value() && actual.code.Value() == expected.code.Value()));
        }
    }  // namespace

    TEST_CASE("Frame evaluation orders sample apply event and camera stages deterministically", "[unit][cinematic][evaluation][order]") {
        std::array<AppliedValue, 8> applied{};
        std::size_t appliedCount = 0;
        TrackProbe transform{Track(30), 30.0F, &applied, &appliedCount};
        TrackProbe propertyB{Track(20), 20.0F, &applied, &appliedCount};
        TrackProbe propertyA{Track(10), 10.0F, &applied, &appliedCount};
        const std::array tracks{SequenceFrameTrackDescriptor{transform.track, SequenceApplyStage::Transform, &transform, SampleTrack,
                                                             ApplyTrack},
                                SequenceFrameTrackDescriptor{propertyB.track, SequenceApplyStage::Property, &propertyB, SampleTrack,
                                                             ApplyTrack},
                                SequenceFrameTrackDescriptor{propertyA.track, SequenceApplyStage::Property, &propertyA, SampleTrack,
                                                             ApplyTrack}};
        const std::array events{SequenceFrameEventKey{Track(9), Key(2), 4, true}, SequenceFrameEventKey{Track(8), Key(1), 4, true}};
        const std::array cuts{SequenceFrameCameraCutKey{Track(40), Key(5), {88, 1}, 5}};
        auto plan = Plan(tracks, events, cuts);
        auto cursor = MakeSequenceFrameCursor(Playing(), SequenceCursorResetPolicy::EmitCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        auto evaluated = plan.Evaluate(Playing(), 5, cursor, scratch.View(), Hooks(hooks));
        REQUIRE(evaluated.HasValue());
        CHECK(evaluated.Value().position == 5);
        CHECK(evaluated.Value().sampledValues == 3);
        CHECK(appliedCount == 3);
        CHECK(applied[0].track == Track(10));
        CHECK(applied[1].track == Track(20));
        CHECK(applied[2].track == Track(30));
        CHECK(applied[0].value == 15.0F);
        REQUIRE(hooks.eventCount == 2);
        CHECK(hooks.events[0].track == Track(8));
        CHECK(hooks.events[1].track == Track(9));
        REQUIRE(hooks.cutCount == 1);
        CHECK(hooks.cuts[0].key == Key(5));
        CHECK(hooks.cuts[0].camera == SequenceCameraTargetId{88, 1});
    }

    TEST_CASE("Frame edges fire exactly once and zero speed stays evaluable", "[unit][cinematic][evaluation][events]") {
        const std::array events{SequenceFrameEventKey{Track(1), Key(1), 0, true}, SequenceFrameEventKey{Track(1), Key(2), 5, true},
                                SequenceFrameEventKey{Track(1), Key(3), 10, true}};
        auto plan = Plan({}, events);
        auto cursor = MakeSequenceFrameCursor(Playing(), SequenceCursorResetPolicy::EmitCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(Playing(), 5, cursor, scratch.View(), Hooks(hooks)).HasValue());
        REQUIRE(hooks.eventCount == 2);
        CHECK(hooks.events[0].key == Key(1));
        CHECK(hooks.events[1].key == Key(2));
        REQUIRE(plan.Evaluate(Playing(), 5, cursor, scratch.View(), Hooks(hooks)).HasValue());
        REQUIRE(hooks.eventCount == 3);
        CHECK(hooks.events[2].key == Key(3));
        const auto zeroRatePlayer = Playing(0, {0, 1}, 3);
        auto zeroRateCursor = MakeSequenceFrameCursor(zeroRatePlayer, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        REQUIRE(plan.Evaluate(zeroRatePlayer, 100, zeroRateCursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(zeroRateCursor.position == 0);
        CHECK(hooks.eventCount == 3);
    }

    TEST_CASE("Reverse intervals use opposite edge inclusion and reverse eligibility", "[unit][cinematic][evaluation][reverse]") {
        const std::array events{SequenceFrameEventKey{Track(1), Key(1), 5, true}, SequenceFrameEventKey{Track(1), Key(2), 7, false},
                                SequenceFrameEventKey{Track(1), Key(3), 10, true}};
        auto plan = Plan({}, events);
        const auto player = Playing(10, {-1, 1});
        auto cursor = MakeSequenceFrameCursor(player, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(player, 5, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(cursor.position == 5);
        REQUIRE(hooks.eventCount == 1);
        CHECK(hooks.events[0].key == Key(1));
        CHECK(hooks.events[0].direction == SequenceTraversalDirection::Reverse);
    }

    TEST_CASE("Rational advancement retains exact fractional remainder across frames", "[unit][cinematic][evaluation][clock]") {
        auto plan = Plan();
        const auto player = Playing(0, {1, 2});
        auto cursor = MakeSequenceFrameCursor(player, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(player, 1, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(cursor.position == 0);
        CHECK(cursor.rateRemainder == 1);
        REQUIRE(plan.Evaluate(player, 1, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(cursor.position == 1);
        CHECK(cursor.rateRemainder == 0);
    }

    TEST_CASE("Loop wrap splits directed intervals and assigns a new traversal", "[unit][cinematic][evaluation][loop]") {
        const std::array events{SequenceFrameEventKey{Track(1), Key(1), 0, true}, SequenceFrameEventKey{Track(1), Key(2), 2, true},
                                SequenceFrameEventKey{Track(1), Key(3), 10, true}};
        auto plan = Plan({}, events, {}, SequenceLoopMode::Loop);
        auto cursor = MakeSequenceFrameCursor(Playing(8), SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(Playing(8), 5, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(cursor.position == 3);
        CHECK(cursor.traversal == 2);
        REQUIRE(hooks.eventCount == 3);
        CHECK(hooks.events[0].key == Key(3));
        CHECK(hooks.events[0].traversal == 1);
        CHECK(hooks.events[1].key == Key(1));
        CHECK(hooks.events[1].traversal == 2);
        CHECK(hooks.events[2].key == Key(2));
    }

    TEST_CASE("Ping-pong reflects without firing the turn boundary twice", "[unit][cinematic][evaluation][pingpong]") {
        const std::array events{SequenceFrameEventKey{Track(1), Key(1), 8, true}, SequenceFrameEventKey{Track(1), Key(2), 10, true}};
        auto plan = Plan({}, events, {}, SequenceLoopMode::PingPong);
        const auto player = Playing(8);
        auto cursor = MakeSequenceFrameCursor(player, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(player, 4, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(cursor.position == 8);
        CHECK(cursor.traversal == 2);
        REQUIRE(hooks.eventCount == 2);
        CHECK(hooks.events[0].key == Key(2));
        CHECK(hooks.events[1].key == Key(1));
        CHECK(hooks.events[1].direction == SequenceTraversalDirection::Reverse);
    }

    TEST_CASE("Seek synchronization is silent and stale control revisions fail atomically",
              "[unit][cinematic][evaluation][seek][lifecycle]") {
        const std::array events{SequenceFrameEventKey{Track(1), Key(1), 3, true}, SequenceFrameEventKey{Track(1), Key(2), 8, true}};
        auto plan = Plan({}, events);
        const auto sought = Playing(8, {1, 1}, 9);
        auto cursor = MakeSequenceFrameCursor(sought, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;

        REQUIRE(plan.Evaluate(sought, 0, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(hooks.eventCount == 0);
        const SequenceFrameCursor before = cursor;
        RequireError(plan.Evaluate(Playing(8, {1, 1}, 10), 1, cursor, scratch.View(), Hooks(hooks)), SequenceEvaluationErrors::Stale);
        CHECK(cursor == before);
        auto paused = sought;
        paused.state = SequencePlaybackState::Paused;
        RequireError(plan.Evaluate(paused, 1, cursor, scratch.View(), Hooks(hooks)), SequenceEvaluationErrors::PlayerStateInvalid);
    }

    TEST_CASE("Failure and capacity preflight publish no values events or cursor changes", "[unit][cinematic][evaluation][failure]") {
        std::array<AppliedValue, 8> applied{};
        std::size_t appliedCount = 0;
        TrackProbe failing{Track(1), 0.0F, &applied, &appliedCount, true};
        const std::array tracks{
            SequenceFrameTrackDescriptor{failing.track, SequenceApplyStage::Property, &failing, SampleTrack, ApplyTrack}};
        const std::array events{SequenceFrameEventKey{Track(2), Key(1), 1, true}};
        auto plan = Plan(tracks, events);
        auto cursor = MakeSequenceFrameCursor(Playing(), SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        const SequenceFrameCursor before = cursor;
        ScratchStorage scratch;
        HookProbe hooks;

        RequireError(plan.Evaluate(Playing(), 1, cursor, scratch.View(), Hooks(hooks)), SequenceEvaluationErrors::Malformed);
        CHECK(cursor == before);
        CHECK(appliedCount == 0);
        CHECK(hooks.eventCount == 0);
        SequenceFrameScratch noEvents{scratch.values, {}, scratch.cuts};
        failing.failSample = false;
        RequireError(plan.Evaluate(Playing(), 1, cursor, noEvents, Hooks(hooks)), SequenceEvaluationErrors::CapacityExceeded);
        CHECK(cursor == before);
        RequireError(plan.Evaluate(Playing(), 1, cursor, scratch.View(), {}), SequenceEvaluationErrors::HookUnavailable);
        CHECK(cursor == before);
    }

    TEST_CASE("Plan activation rejects malformed identities collisions and bounds", "[unit][cinematic][evaluation][validation]") {
        std::array<AppliedValue, 8> applied{};
        std::size_t appliedCount = 0;
        TrackProbe first{Track(1), 0.0F, &applied, &appliedCount};
        TrackProbe second{Track(1), 1.0F, &applied, &appliedCount};
        const std::array duplicateTracks{SequenceFrameTrackDescriptor{first.track, SequenceApplyStage::Property, &first, SampleTrack,
                                                                      ApplyTrack},
                                         SequenceFrameTrackDescriptor{second.track, SequenceApplyStage::Transform, &second, SampleTrack,
                                                                      ApplyTrack}};
        RequireError(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 1, duplicateTracks, {}, {}),
                     SequenceEvaluationErrors::Malformed);
        const std::array duplicateEvents{SequenceFrameEventKey{Track(2), Key(3), 2, true},
                                         SequenceFrameEventKey{Track(2), Key(3), 4, true}};
        RequireError(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 1, {}, duplicateEvents, {}),
                     SequenceEvaluationErrors::Malformed);
        const std::array invalidCut{SequenceFrameCameraCutKey{Track(4), Key(5), {}, 4}};
        RequireError(SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 1, {}, {}, invalidCut),
                     SequenceEvaluationErrors::Malformed);
        RequireError(SequenceFrameEvaluationPlan::Create(0, SequenceLoopMode::Once, 1, {}, {}, {}), SequenceEvaluationErrors::Malformed);
    }

    TEST_CASE("Player ordering ignores input order and rejects duplicate handles", "[unit][cinematic][evaluation][batch]") {
        const std::array input{SequenceFramePlayerOrder{Handle(30), 4}, SequenceFramePlayerOrder{Handle(20), 9},
                               SequenceFramePlayerOrder{Handle(10), 9}};
        std::array<SequenceFramePlayerOrder, 3> output{};
        auto ordered = OrderSequenceFramePlayers(input, output);
        REQUIRE(ordered.HasValue());
        CHECK(output[0].player.player == SequencePlayerId{10, 1});
        CHECK(output[1].player.player == SequencePlayerId{20, 1});
        CHECK(output[2].player.player == SequencePlayerId{30, 1});
        const std::array duplicate{input[0], input[0]};
        RequireError(OrderSequenceFramePlayers(duplicate, output), SequenceEvaluationErrors::Malformed);
        const std::array reprioritizedDuplicate{SequenceFramePlayerOrder{Handle(30), 100}, input[1], input[0]};
        RequireError(OrderSequenceFramePlayers(reprioritizedDuplicate, output), SequenceEvaluationErrors::Malformed);
    }

    TEST_CASE("Steady-state evaluation is allocation-free and bounded", "[unit][cinematic][evaluation][allocation]") {
        auto plan = Plan();
        auto cursor = MakeSequenceFrameCursor(Playing(), SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        ScratchStorage scratch;
        HookProbe hooks;
        const std::size_t before = Tests::AllocationProbe::Count();
        for (std::size_t frame = 0; frame < 1'000; ++frame)
            REQUIRE(plan.Evaluate(Playing(), 0, cursor, scratch.View(), Hooks(hooks)).HasValue());
        CHECK(Tests::AllocationProbe::Count() == before);

        auto bounded = Plan({}, {}, {}, SequenceLoopMode::Loop, 1);
        const auto player = Playing();
        auto boundedCursor = MakeSequenceFrameCursor(player, SequenceCursorResetPolicy::SuppressCurrentBoundary).Value();
        const SequenceFrameCursor unchanged = boundedCursor;
        RequireError(bounded.Evaluate(player, 25, boundedCursor, scratch.View(), Hooks(hooks)), SequenceEvaluationErrors::CapacityExceeded);
        CHECK(boundedCursor == unchanged);
    }

    TEST_CASE("Evaluation errors use stable unique identities", "[unit][cinematic][evaluation][errors]") {
        const std::array descriptors{&SequenceEvaluationErrors::Malformed,          &SequenceEvaluationErrors::Stale,
                                     &SequenceEvaluationErrors::PlayerStateInvalid, &SequenceEvaluationErrors::DeltaInvalid,
                                     &SequenceEvaluationErrors::ArithmeticOverflow, &SequenceEvaluationErrors::CapacityExceeded,
                                     &SequenceEvaluationErrors::HookUnavailable,    &SequenceEvaluationErrors::RevisionExhausted};
        std::set<std::string_view> codes;
        for (const auto *descriptor : descriptors) {
            CHECK(descriptor->domain.Value() == "horo.cinematic.evaluation");
            CHECK(codes.insert(descriptor->code.Value()).second);
            CHECK_FALSE(descriptor->summary.empty());
            CHECK_FALSE(descriptor->remediationHint.empty());
        }
    }
}  // namespace Horo::Cinematic
