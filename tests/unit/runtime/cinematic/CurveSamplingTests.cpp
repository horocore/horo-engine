#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Cinematic/CurveSampling.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

namespace Horo::Cinematic {
    namespace {
        using Catch::Approx;

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
            CHECK(result.ErrorValue().severity == descriptor.defaultSeverity);
        }

        [[nodiscard]] constexpr ScalarCurveKey Key(const CurveTime time, const float value,
                                                   const CurveInterpolation interpolation = CurveInterpolation::Linear) noexcept {
            return {.time = time, .value = value, .interpolation = interpolation};
        }

        [[nodiscard]] std::array<ScalarCurveKey, 2> RepeatabilityFixture() {
            std::array keys{Key(0, 1.0F, CurveInterpolation::CubicBezier), Key(10, 9.0F)};
            keys.front().tangentOut = {3, 4.0F};
            keys.back().tangentIn = {-3, -2.0F};
            return keys;
        }

        float Sample(const ScalarCurveView &curve, const CurveTime time) {
            auto sample = curve.Sample(time);
            REQUIRE(sample.HasValue());
            return sample.Value().value;
        }
    }  // namespace

    TEST_CASE("Scalar curve interpolation modes use deterministic segment-local math", "[unit][cinematic][curve]") {
        SECTION("constant and linear") {
            std::array keys{Key(10, 2.0F, CurveInterpolation::Constant), Key(20, 8.0F)};
            auto constant = ScalarCurveView::Create(keys);
            REQUIRE(constant.HasValue());
            CHECK(Sample(constant.Value(), 15) == 2.0F);
            keys[0].interpolation = CurveInterpolation::Linear;
            auto linear = ScalarCurveView::Create(keys);
            REQUIRE(linear.HasValue());
            CHECK(Sample(linear.Value(), 15) == 5.0F);
        }

        SECTION("cubic bezier and hermite") {
            std::array bezier{Key(0, 0.0F, CurveInterpolation::CubicBezier), Key(12, 12.0F)};
            bezier[0].tangentOut = {4, 4.0F};
            bezier[1].tangentIn = {-4, -4.0F};
            auto cubic = ScalarCurveView::Create(bezier);
            REQUIRE(cubic.HasValue());
            CHECK(Sample(cubic.Value(), 6) == Approx(6.0F).margin(0.005F));

            std::array hermite{Key(0, 0.0F, CurveInterpolation::HermiteSpline), Key(12, 12.0F)};
            hermite[0].tangentOut = {4, 4.0F};
            hermite[1].tangentIn = {-4, -4.0F};
            auto spline = ScalarCurveView::Create(hermite);
            REQUIRE(spline.HasValue());
            CHECK(Sample(spline.Value(), 6) == Approx(6.0F));
        }
    }

    TEST_CASE("Scalar curve random access is independent of seek history", "[unit][cinematic][curve][seek]") {
        const std::array keys{Key(0, 0.0F), Key(10, 100.0F), Key(20, 0.0F)};
        auto curve = ScalarCurveView::Create(keys);
        REQUIRE(curve.HasValue());
        const float direct = Sample(curve.Value(), 7);
        CHECK(Sample(curve.Value(), 19) == 10.0F);
        CHECK(Sample(curve.Value(), 2) == 20.0F);
        CHECK(Sample(curve.Value(), 7) == direct);
        CHECK(direct == 70.0F);
    }

    TEST_CASE("Scalar curve boundary policies map large forward and reverse seeks", "[unit][cinematic][curve][boundary]") {
        const std::array keys{Key(10, 10.0F), Key(20, 20.0F)};
        auto clamped = ScalarCurveView::Create(keys);
        REQUIRE(clamped.HasValue());
        CHECK(Sample(clamped.Value(), std::numeric_limits<CurveTime>::min()) == 10.0F);
        CHECK(Sample(clamped.Value(), std::numeric_limits<CurveTime>::max()) == 20.0F);

        auto repeated = ScalarCurveView::Create(keys, {CurveBoundaryMode::Repeat, CurveBoundaryMode::Repeat});
        REQUIRE(repeated.HasValue());
        CHECK(Sample(repeated.Value(), 5) == 15.0F);
        CHECK(Sample(repeated.Value(), 25) == 15.0F);
        CHECK(Sample(repeated.Value(), 30) == 10.0F);

        auto pingPong = ScalarCurveView::Create(keys, {CurveBoundaryMode::PingPong, CurveBoundaryMode::PingPong});
        REQUIRE(pingPong.HasValue());
        CHECK(Sample(pingPong.Value(), 5) == 15.0F);
        CHECK(Sample(pingPong.Value(), 25) == 15.0F);
        CHECK(Sample(pingPong.Value(), 30) == 10.0F);
        CHECK(Sample(pingPong.Value(), 35) == 15.0F);
        CHECK(Sample(pingPong.Value(), -5) == 15.0F);

        auto asymmetric = ScalarCurveView::Create(keys, {CurveBoundaryMode::Clamp, CurveBoundaryMode::Repeat});
        REQUIRE(asymmetric.HasValue());
        CHECK(asymmetric.Value().Boundaries() == CurveBoundaryPolicy{CurveBoundaryMode::Clamp, CurveBoundaryMode::Repeat});
        CHECK(Sample(asymmetric.Value(), 5) == 10.0F);
        CHECK(Sample(asymmetric.Value(), 25) == 15.0F);
    }

    TEST_CASE("Scalar curve validation rejects hostile and ambiguous inputs", "[unit][cinematic][curve][validation]") {
        RequireError(ScalarCurveView::Create({}), CinematicErrors::CurveMalformed);
        const std::array duplicate{Key(2, 1.0F), Key(2, 2.0F)};
        RequireError(ScalarCurveView::Create(duplicate), CinematicErrors::CurveMalformed);
        const std::array negative{Key(-1, 1.0F)};
        RequireError(ScalarCurveView::Create(negative), CinematicErrors::CurveMalformed);

        auto nonFinite = Key(0, std::numeric_limits<float>::quiet_NaN());
        RequireError(ScalarCurveView::Create(std::span{&nonFinite, 1}), CinematicErrors::CurveNonFinite);
        nonFinite = Key(0, 0.0F);
        nonFinite.tangentOut.valueOffset = std::numeric_limits<float>::infinity();
        RequireError(ScalarCurveView::Create(std::span{&nonFinite, 1}), CinematicErrors::CurveNonFinite);

        std::array crossing{Key(0, 0.0F, CurveInterpolation::CubicBezier), Key(10, 1.0F)};
        crossing[0].tangentOut = {8, 1.0F};
        crossing[1].tangentIn = {-8, -1.0F};
        RequireError(ScalarCurveView::Create(crossing), CinematicErrors::CurveTangentInvalid);
        crossing[0].interpolation = CurveInterpolation::HermiteSpline;
        crossing[0].tangentOut.timeOffset = 0;
        RequireError(ScalarCurveView::Create(crossing), CinematicErrors::CurveTangentInvalid);

        const std::array keys{Key(0, 0.0F), Key(1, 1.0F)};
        RequireError(ScalarCurveView::Create(keys, {CurveBoundaryMode::Count, CurveBoundaryMode::Clamp}), CinematicErrors::CurveMalformed);
        auto unknownInterpolation = keys;
        unknownInterpolation[0].interpolation = CurveInterpolation::Count;
        RequireError(ScalarCurveView::Create(unknownInterpolation), CinematicErrors::CurveMalformed);

        const std::array invalidTangents{CurveTangent{-1, 0.0F}, CurveTangent{11, 0.0F}, CurveTangent{1, 0.0F}, CurveTangent{-11, 0.0F}};
        for (std::size_t index = 0; index < invalidTangents.size(); ++index) {
            std::array invalid{Key(0, 0.0F, CurveInterpolation::CubicBezier), Key(10, 1.0F)};
            if (index < 2)
                invalid[0].tangentOut = invalidTangents[index];
            else
                invalid[1].tangentIn = invalidTangents[index];
            RequireError(ScalarCurveView::Create(invalid), CinematicErrors::CurveTangentInvalid);
        }

        std::array invalidHermite{Key(0, 0.0F, CurveInterpolation::HermiteSpline), Key(10, 1.0F)};
        invalidHermite[0].tangentOut = {1, 1.0F};
        invalidHermite[1].tangentIn = {0, -1.0F};
        RequireError(ScalarCurveView::Create(invalidHermite), CinematicErrors::CurveTangentInvalid);

        std::array overflow{Key(0, std::numeric_limits<float>::max(), CurveInterpolation::CubicBezier),
                            Key(10, std::numeric_limits<float>::max())};
        overflow[0].tangentOut = {3, std::numeric_limits<float>::max()};
        overflow[1].tangentIn = {-3, 0.0F};
        auto admittedOverflow = ScalarCurveView::Create(overflow);
        REQUIRE(admittedOverflow.HasValue());
        RequireError(admittedOverflow.Value().Sample(5), CinematicErrors::CurveNonFinite);
    }

    TEST_CASE("Scalar curve returns exact keys and repeatable bits", "[unit][cinematic][curve][determinism]") {
        auto keys = RepeatabilityFixture();
        auto curve = ScalarCurveView::Create(keys);
        REQUIRE(curve.HasValue());
        CHECK(Sample(curve.Value(), 0) == 1.0F);
        CHECK(Sample(curve.Value(), 10) == 9.0F);
        const float expected = Sample(curve.Value(), 4);
        for (std::size_t iteration = 0; iteration < 1'000; ++iteration)
            REQUIRE(Sample(curve.Value(), 4) == expected);

        const std::array single{Key(8, 42.0F)};
        auto degenerate = ScalarCurveView::Create(single, {CurveBoundaryMode::Repeat, CurveBoundaryMode::PingPong});
        REQUIRE(degenerate.HasValue());
        auto degenerateSample = degenerate.Value().Sample(-100);
        REQUIRE(degenerateSample.HasValue());
        CHECK(degenerateSample.Value() == ScalarCurveSample{42.0F, 8, 0, true});
    }

    TEST_CASE("Scalar curve steady-state sampling performs no heap allocation", "[unit][cinematic][curve][allocation]") {
        auto keys = RepeatabilityFixture();
        auto curve = ScalarCurveView::Create(keys);
        REQUIRE(curve.HasValue());
        static_cast<void>(curve.Value().Sample(4));
        const std::size_t before = Tests::AllocationProbe::Count();
        for (std::size_t iteration = 0; iteration < 10'000; ++iteration) {
            auto sample = curve.Value().Sample(static_cast<CurveTime>(iteration % 11));
            REQUIRE(sample.HasValue());
        }
        CHECK(Tests::AllocationProbe::Count() == before);
    }

    TEST_CASE("Scalar curve replay is order independent within the zero-allocation budget", "[unit][cinematic][curve][qualification]") {
        const auto keys = RepeatabilityFixture();
        const auto admitted = ScalarCurveView::Create(keys, {CurveBoundaryMode::Repeat, CurveBoundaryMode::PingPong});
        REQUIRE(admitted.HasValue());
        const ScalarCurveView &curve = admitted.Value();
        std::array<ScalarCurveSample, 256> baseline{};
        for (std::size_t index = 0; index < baseline.size(); ++index) {
            const CurveTime time = static_cast<CurveTime>((index * 73U) % 511U) - 255;
            const auto sample = curve.Sample(time);
            REQUIRE(sample.HasValue());
            baseline[index] = sample.Value();
        }

        const std::size_t before = Tests::AllocationProbe::Count();
        for (std::size_t replay = 0; replay < 16; ++replay) {
            for (std::size_t offset = 0; offset < baseline.size(); ++offset) {
                const std::size_t index = (offset * 73U + replay) % baseline.size();
                const CurveTime time = static_cast<CurveTime>((index * 73U) % 511U) - 255;
                const auto sample = curve.Sample(time);
                REQUIRE(sample.HasValue());
                CHECK(sample.Value() == baseline[index]);
            }
        }
        CHECK(Tests::AllocationProbe::Count() == before);
    }

    TEST_CASE("Scalar curve admits the exact key ceiling and rejects one over", "[unit][cinematic][curve][capacity]") {
        std::vector<ScalarCurveKey> keys(MaximumScalarCurveKeys + 1);
        for (std::size_t index = 0; index < keys.size(); ++index)
            keys[index] = Key(static_cast<CurveTime>(index), static_cast<float>(index));
        REQUIRE(ScalarCurveView::Create(std::span{keys}.first(MaximumScalarCurveKeys)).HasValue());
        RequireError(ScalarCurveView::Create(keys), CinematicErrors::CurveLimitExceeded);
    }

    TEST_CASE("Scalar curve errors expose stable actionable identities", "[unit][cinematic][curve][errors]") {
        CHECK(CinematicErrors::CurveMalformed.code.Value() == "cinematic.curve.malformed");
        CHECK(CinematicErrors::CurveNonFinite.code.Value() == "cinematic.curve.non_finite");
        CHECK(CinematicErrors::CurveTangentInvalid.code.Value() == "cinematic.curve.tangent_invalid");
        CHECK(CinematicErrors::CurveLimitExceeded.code.Value() == "cinematic.curve.limit_exceeded");
        CHECK(CinematicErrors::CurveMalformed.userActionable);
        CHECK(CinematicErrors::CurveNonFinite.userActionable);
        CHECK(CinematicErrors::CurveTangentInvalid.userActionable);
        CHECK(CinematicErrors::CurveLimitExceeded.userActionable);
    }
}  // namespace Horo::Cinematic
