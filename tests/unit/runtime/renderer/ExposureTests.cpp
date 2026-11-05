#include "ColorPipelineTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;
}  // namespace

TEST_CASE("Exposure stops preserve the normative sign and reject nonfinite or unbounded values", "[renderer][exposure]") {
    CHECK(ReferenceExposureScale(1).Value() == Catch::Approx(2));
    CHECK(ReferenceExposureScale(-1).Value() == Catch::Approx(0.5));
    CHECK(ReferenceExposureScale(0).Value() == Catch::Approx(1));
    const float invalid = GENERATE(-33.0F, 33.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
    REQUIRE(ReferenceExposureScale(invalid).HasError());
}

TEST_CASE("Exposure authored floating point settings reject NaN independently", "[renderer][exposure]") {
    const auto member =
        GENERATE(&ExposureSettings::manualEv, &ExposureSettings::compensationEv, &ExposureSettings::initialEv, &ExposureSettings::minimumEv,
                 &ExposureSettings::maximumEv, &ExposureSettings::minimumLogLuminance, &ExposureSettings::maximumLogLuminance,
                 &ExposureSettings::lowPercentile, &ExposureSettings::highPercentile, &ExposureSettings::neutralTarget,
                 &ExposureSettings::brightenEvPerSecond, &ExposureSettings::darkenEvPerSecond);
    ExposureSettings settings;
    REQUIRE(ValidateExposureSettings(settings).HasValue());
    settings.*member = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(ValidateExposureSettings(settings).HasError());
}

TEST_CASE("Exposure rejects unknown mode time and reset values", "[renderer][exposure]") {
    ExposureSettings settings;
    SECTION("metering") {
        settings.mode = static_cast<ExposureMode>(255);
    }
    SECTION("clock") {
        settings.timeSource = static_cast<ExposureTimeSource>(255);
    }
    SECTION("reset") {
        settings.reset = static_cast<ExposureResetPolicy>(255);
    }
    REQUIRE(ValidateExposureSettings(settings).HasError());
}

TEST_CASE("Exposure dispatch admits an exact bounded region without reading scene pixels", "[renderer][exposure]") {
    ExposureSettings settings;
    settings.mode = ExposureMode::AutomaticHistogram;
    settings.compensationEv = 2;
    const std::array<std::uint32_t, 4> region{3, 4, 10, 9};
    auto parameters = PrepareExposureDispatch(settings, {16, 16}, region, 0.25F);
    REQUIRE(parameters.HasValue());
    CHECK(parameters.Value().region == region);
    CHECK(parameters.Value().timing[0] == 0.25F);
    CHECK(parameters.Value().timing[1] == 0.18F);
    CHECK(parameters.Value().timing[2] == 2);
    CHECK(parameters.Value().adaptation[2] == settings.brightenEvPerSecond);
    CHECK(parameters.Value().controls[0] == 1);
    CHECK(parameters.Value().controls[1] == ExposureHistogramBins);
    CHECK(ExposureHistogramBins == 256);
}

TEST_CASE("Exposure regions reject empty out of bounds and overflowing coordinates", "[renderer][exposure]") {
    const auto region = GENERATE((std::array<std::uint32_t, 4>{0, 0, 0, 1}), (std::array<std::uint32_t, 4>{0, 0, 1, 0}),
                                 (std::array<std::uint32_t, 4>{16, 0, 1, 1}), (std::array<std::uint32_t, 4>{0, 16, 1, 1}),
                                 (std::array<std::uint32_t, 4>{15, 0, 2, 1}),
                                 (std::array<std::uint32_t, 4>{std::numeric_limits<std::uint32_t>::max(), 0, 2, 1}));
    ExposureSettings settings;
    settings.mode = ExposureMode::AutomaticHistogram;
    REQUIRE(PrepareExposureDispatch(settings, {16, 16}, region, 0.01F).HasError());
}

TEST_CASE("Exposure dispatch rejects invalid time without inventing a different clock", "[renderer][exposure]") {
    const float delta = GENERATE(-1.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN());
    ExposureSettings settings;
    settings.mode = ExposureMode::AutomaticHistogram;
    REQUIRE(PrepareExposureDispatch(settings, {16, 16}, {0, 0, 16, 16}, 0).HasValue());
    REQUIRE(PrepareExposureDispatch(settings, {16, 16}, {0, 0, 16, 16}, delta).HasError());
}

TEST_CASE("Exposure work bounds reject oversized histograms before dispatch packing", "[renderer][exposure]") {
    ExposureSettings settings;
    settings.mode = ExposureMode::AutomaticHistogram;
    REQUIRE(PrepareExposureDispatch(settings, {4096, 4096}, {0, 0, 4096, 4096}, 1).HasValue());
    CHECK(PrepareExposureDispatch(settings, {4097, 4096}, {0, 0, 4097, 4096}, 1).HasError());
}

TEST_CASE("Automatic exposure metadata consumes exactly the next submission of the same view", "[renderer][exposure]") {
    ColorPipelineFixture fixture;
    auto state = fixture.State();
    state.mode = ExposureMode::AutomaticHistogram;
    state.buffer = {{1}, 8, 1};
    state.sourceSubmission = 8;
    state.consumeSubmission = 9;
    state.diagnosticEv.reset();
    REQUIRE(ValidateExposureState(state, fixture.source.compatibility, 7, 9).HasValue());
    CHECK(ValidateExposureState(state, fixture.source.compatibility, 7, 8).HasError());
    CHECK(ValidateExposureState(state, fixture.source.compatibility, 7, 10).HasError());
    ++state.view.value;
    CHECK(ValidateExposureState(state, fixture.source.compatibility, 7, 9).HasError());
}

TEST_CASE("Exposure rejects stale resource settings and generation evidence", "[renderer][exposure]") {
    ColorPipelineFixture fixture;
    auto state = fixture.State();
    SECTION("settings") {
        ++state.settingsGeneration;
    }
    SECTION("exposure generation") {
        ++state.generation;
    }
    SECTION("device") {
        ++state.deviceGeneration;
    }
    SECTION("manual cannot claim a GPU resource") {
        state.buffer = {{1}, 8, 1};
    }
    SECTION("diagnostic nonfinite") {
        state.diagnosticEv = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("reason vocabulary") {
        state.reason = static_cast<ExposureSampleReason>(255);
    }
    REQUIRE(ValidateExposureState(state, fixture.source.compatibility, 7, 1).HasError());
}

TEST_CASE("Automatic exposure cannot invent a predecessor across overflow or absent GPU storage", "[renderer][exposure]") {
    ColorPipelineFixture fixture;
    auto state = fixture.State();
    state.mode = ExposureMode::AutomaticHistogram;
    state.buffer = {{1}, 8, 1};
    state.sourceSubmission = 8;
    state.consumeSubmission = 9;
    REQUIRE(ValidateExposureState(state, fixture.source.compatibility, 7, 9).HasValue());
    SECTION("missing resource") {
        state.buffer = {};
    }
    SECTION("wrapped predecessor") {
        state.sourceSubmission = std::numeric_limits<std::uint64_t>::max();
    }
    REQUIRE(ValidateExposureState(state, fixture.source.compatibility, 7, 9).HasError());
}
