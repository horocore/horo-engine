#include "AnimationClipNotifyTestFixtures.h"
#include "Horo/Animation/AnimationCompression.h"

#include <cmath>
#include <limits>

namespace Horo::Animation::Test {
    using namespace Notifies;

    TEST_CASE("Clip event schemas bind exact owned typed payloads and canonical order", "[animation][clip][notifies]") {
        Instance::Prepared fixture;
        auto data = fixture.clips[0]->Data();
        data.eventSchemas = {Schema()};
        data.events = {Event(2, 100), Event(1, 0, AnimationDeltaTime{10})};
        auto created = AnimationClipAsset::Create(data, *fixture.skeleton);
        REQUIRE(created.HasValue());
        CHECK(created.Value().Data().events[0].id == Id<AnimationEventMarkerId>(1));
        data.events[1].payload.fields[0].value = std::uint64_t{0};
        CHECK(std::get<std::uint64_t>(created.Value().Data().events[0].payload.fields[0].value) == 1);
    }

    TEST_CASE("Clip events reject malformed schemas fields identities and endpoint windows", "[animation][clip][notifies]") {
        Instance::Prepared fixture;
        auto data = fixture.clips[0]->Data();
        data.eventSchemas = {Schema()};
        data.events = {Event(1, 10, AnimationDeltaTime{10})};
        SECTION("required missing") {
            data.events[0].payload = {};
        }
        SECTION("wrong typed value") {
            data.events[0].payload.fields[0].value = std::int64_t{1};
        }
        SECTION("unknown field") {
            data.events[0].payload.fields[0].id = Id<AnimationEventFieldId>(2);
        }
        SECTION("outside typed registry") {
            data.events[0].payload.fields[0].value = std::uint64_t{2};
        }
        SECTION("nonfinite number") {
            data.eventSchemas[0].fields[0].kind = AnimationEventValueKind::Number;
            data.eventSchemas[0].fields[0].allowedUnsigned.clear();
            data.events[0].payload.fields[0].value = std::numeric_limits<double>::quiet_NaN();
        }
        SECTION("nonfinite vector") {
            data.eventSchemas[0].fields[0].kind = AnimationEventValueKind::Vector3;
            data.eventSchemas[0].fields[0].allowedUnsigned.clear();
            data.events[0].payload.fields[0].value = Math::Vec3{std::numeric_limits<float>::infinity(), 0, 0};
        }
        SECTION("invalid asset identity") {
            data.eventSchemas[0].fields[0].kind = AnimationEventValueKind::Asset;
            data.eventSchemas[0].fields[0].allowedUnsigned.clear();
            data.events[0].payload.fields[0].value = Assets::AssetId{};
        }
        SECTION("duplicate field") {
            data.events[0].payload.count = 2;
            data.events[0].payload.fields[1] = data.events[0].payload.fields[0];
        }
        SECTION("unknown schema") {
            data.events[0].schemaRevision = 2;
        }
        SECTION("duplicate marker") {
            data.events.push_back(data.events[0]);
        }
        SECTION("duplicate schema") {
            data.eventSchemas.push_back(data.eventSchemas[0]);
        }
        SECTION("negative point") {
            data.events[0].time.ticks = -1;
        }
        SECTION("overflowing window") {
            data.events[0].duration = AnimationDeltaTime{std::numeric_limits<std::int64_t>::max()};
        }
        SECTION("zero window") {
            data.events[0].duration = AnimationDeltaTime{0};
        }
        SECTION("beyond duration") {
            data.events[0].time.ticks = 101;
        }
        SECTION("empty name") {
            data.events[0].name.clear();
        }
        SECTION("hidden nondefault tail") {
            data.events[0].payload.fields[7].value = std::uint64_t{3};
        }
        CHECK(AnimationClipAsset::Create(data, *fixture.skeleton).HasError());
    }

    TEST_CASE("Clip trace conserves canonical regional traversal and exact directed seams without allocation",
              "[animation][clip][notifies]") {
        Instance::Prepared fixture;
        const auto &clip = *fixture.clips[0];
        const AnimationClipRegion region{{20}, {80}, AnimationWrapMode::Loop};
        std::array<AnimationClipTraversalSegment, 16> output;
        const auto before = HoroAnimationTestAllocationCount();
        const auto expected = clip.TraverseRegionPrepared({}, {180}, region);
        const auto trace = clip.TraceRegionPrepared({}, {180}, region, output);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(before == after);
        REQUIRE(expected.HasValue());
        REQUIRE(trace.HasValue());
        CHECK(trace.Value().traversal.cursor == expected.Value().cursor);
        CHECK(trace.Value().traversal.sampleTime == expected.Value().sampleTime);
        CHECK(trace.Value().traversal.boundaryCrossings == expected.Value().boundaryCrossings);
        REQUIRE(trace.Value().segments == 3);
        for (std::uint32_t i = 0; i < 3; ++i) {
            CHECK(output[i].from.ticks == 20);
            CHECK(output[i].to.ticks == 80);
            CHECK(output[i].loopSeamAfter);
        }
    }

    TEST_CASE("Clip trace handles reverse turns terminal hold and empty output atomically", "[animation][clip][notifies]") {
        Instance::Prepared fixture;
        const auto &clip = *fixture.clips[0];
        std::array<AnimationClipTraversalSegment, 8> output;
        SECTION("reverse loop departs zero through prior end") {
            const auto trace = clip.TraceRegionPrepared({}, {-20}, {{20}, {80}, AnimationWrapMode::Loop}, output);
            REQUIRE(trace.HasValue());
            REQUIRE(trace.Value().segments == 1);
            CHECK(output[0].loopSeamBefore);
            CHECK(output[0].from.ticks == 80);
            CHECK(output[0].to.ticks == 60);
        }
        SECTION("ping pong exact turn") {
            const auto trace = clip.TraceRegionPrepared({}, {150}, {{0}, {100}, AnimationWrapMode::PingPong}, output);
            REQUIRE(trace.HasValue());
            REQUIRE(trace.Value().segments == 2);
            CHECK(output[0].to.ticks == 100);
            CHECK(output[1].from.ticks == 100);
            CHECK(output[1].to.ticks == 50);
            CHECK(output[1].direction == AnimationTraversalDirection::Reverse);
        }
        SECTION("terminal hold") {
            const auto terminal = clip.TraverseRegionPrepared({}, {100}, {{20}, {80}, AnimationWrapMode::Once});
            REQUIRE(terminal.HasValue());
            const auto trace = clip.TraceRegionPrepared(terminal.Value().cursor, {-10}, {{20}, {80}, AnimationWrapMode::Once}, output);
            REQUIRE(trace.HasValue());
            CHECK(trace.Value().segments == 0);
        }
        SECTION("failure leaves caller output untouched") {
            output[0].from = {123};
            const auto trace = clip.TraceRegionPrepared({}, {150}, {{0}, {100}, AnimationWrapMode::Loop}, std::span{output}.first(1));
            CHECK(trace.HasError());
            CHECK(output[0].from.ticks == 123);
        }
    }

    TEST_CASE("Clip trace preserves admission precedence and complete crossing budget before output writes",
              "[animation][clip][notifies]") {
        Instance::Prepared fixture;
        const auto &clip = *fixture.clips[0];
        std::array<AnimationClipTraversalSegment, 8> output;
        output[0].from = {999};
        AnimationClipLimits limits;
        limits.maximumBoundaryCrossings = 0;
        const auto before = HoroAnimationTestAllocationCount();
        const auto limited = clip.TraceRegionPrepared({}, {200}, {{0}, {100}, AnimationWrapMode::Loop}, output, limits);
        const auto cancelled = clip.TraceRegionPrepared({}, {1}, {{-1}, {100}, AnimationWrapMode::Loop}, output, {},
                                                        AnimationClipAdmissionState::CancellationRequested);
        const auto after = HoroAnimationTestAllocationCount();
        CHECK(before == after);
        REQUIRE(limited.HasError());
        CHECK(limited.ErrorValue().failure == AnimationClipFailure::LimitExceeded);
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().failure == AnimationClipFailure::Cancelled);
        CHECK(output[0].from.ticks == 999);
    }

    TEST_CASE("Compression retains actual immutable event schema payload and clip revision", "[animation][clip][notifies][compression]") {
        Instance::Prepared fixture;
        Events(fixture, 0, {Event(1, 10), Event(2, 20, AnimationDeltaTime{30})});
        const auto &source = *fixture.clips[0];
        const auto profile = GetAnimationCompressionCookProfile(AnimationCompressionCookTier::Lossless);
        REQUIRE(profile.HasValue());
        const auto cooked = CookAnimationClip(source, *fixture.skeleton, profile.Value(),
                                              {.clipGeneration = source.Data().descriptor.generation,
                                               .skeletonGeneration = source.Data().descriptor.skeletonGeneration});
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value().Clip().Data().eventSchemas == source.Data().eventSchemas);
        CHECK(cooked.Value().Clip().Data().events == source.Data().events);
        CHECK(cooked.Value().Clip().Data().descriptor.generation == source.Data().descriptor.generation);
    }

    TEST_CASE("Clip event payload vocabulary retains exact scalar vector and asset values", "[animation][clip][notifies][payload]") {
        Instance::Prepared fixture;
        auto data = fixture.clips[0]->Data();
        AnimationEventSchema schema{Id<AnimationEventTypeId>(40), 1};
        auto event = Event(1, 10);
        event.payload = {};
        event.payload.count = 6;
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 9;
        const std::array<AnimationEventValue, 6> values{true, std::int64_t{-9},    std::uint64_t{9},
                                                        -0.0, Math::Vec3{1, 2, 3}, Assets::AssetId::FromBytes(bytes)};
        for (std::uint64_t i = 0; i < values.size(); ++i) {
            const auto field = Id<AnimationEventFieldId>(i + 1);
            schema.fields.push_back({field, static_cast<AnimationEventValueKind>(i)});
            event.payload.fields[i] = {field, values[i]};
        }
        data.eventSchemas = {schema};
        data.events = {event};
        const auto created = AnimationClipAsset::Create(data, *fixture.skeleton);
        REQUIRE(created.HasValue());
        CHECK(created.Value().Data().events[0].payload == event.payload);
        CHECK(std::signbit(std::get<double>(created.Value().Data().events[0].payload.fields[3].value)));
    }

}  // namespace Horo::Animation::Test
