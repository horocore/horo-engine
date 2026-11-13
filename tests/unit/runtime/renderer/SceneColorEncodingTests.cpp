#include "Horo/Runtime/Render/SceneColorEncoding.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr SceneColorSourcePolicy Linear{SceneColorEncoding::LegacyLinearRec709D65, 1};
    constexpr SceneColorSourcePolicy Srgb{SceneColorEncoding::SrgbRec709D65, 1};
    constexpr SceneColorSourcePolicy Canonical{SceneColorEncoding::LinearAcesCg, 1};

    /** @brief Compares independently pinned color components with an explicit absolute error budget. */
    void CheckColor(Math::Vec3 actual, Math::Vec3 expected, double tolerance) {
        CHECK(actual.x == Catch::Approx(expected.x).epsilon(0).margin(tolerance));
        CHECK(actual.y == Catch::Approx(expected.y).epsilon(0).margin(tolerance));
        CHECK(actual.z == Catch::Approx(expected.z).epsilon(0).margin(tolerance));
    }

    /** @brief Preserves typed failure identity without dereferencing a failed conversion. */
    void CheckFailure(const Result<Math::Vec3> &result, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
    }

    /** @brief One pinned independent processor observation and its explicit absolute comparison budget. */
    struct Golden {
        Math::Vec3 input, output;
        double tolerance;
    };

    // Independent OpenColorIO 2.5.0 CG config4 CPU processor observations from scene-ingress-reference.json.
    // Config SHA256: 9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff.
    // Literals keep owning tests independent of a developer's Python/OCIO installation and filesystem.
    constexpr std::array LinearGoldens{Golden{{0, 0, 0}, {0, 0, 0}, 0}, Golden{{1, 1, 1}, {1, 1, 1}, 1.2e-7},
                                       Golden{{-1, 0, 0}, {-0.6130974292755127F, -0.07019372284412384F, -0.0206155925989151F}, 0},
                                       Golden{{-0.25F, 0.18F, 4}, {0.0973576158285141F, 0.20120488107204437F, 3.4938271045684814F}, 2.4e-7},
                                       Golden{{65504, 0, 0}, {40160.3359375F, 4597.9697265625F, 1350.40380859375F}, 0}};

    // OCIO's default CPU power approximation differs from the analytic EOTF: ~4.3e-6 at0.5 and ~6.9e-5 at2.
    // The pinned processor also differs from the standard negative linear extension by9.61e-6 at-0.5.
    // Each vector has its own allowance; a separate analytic test tightly enforces the required negative extension.
    constexpr std::array
        SrgbGoldens{Golden{{-0.5F, -0.5F, -0.5F}, {-0.038690079003572464F, -0.038690079003572464F, -0.038690079003572464F}, 1e-5},
                    Golden{{-0.01F, -0.01F, -0.01F}, {-0.0007738015265204012F, -0.0007738015847280622F, -0.0007738015847280622F}, 2e-7},
                    Golden{{0.04045F, 0.04045F, 0.04045F}, {0.0031307393219321966F, 0.0031307393219321966F, 0.0031307395547628403F}, 7e-8},
                    Golden{{0.5F, 0.5F, 0.5F}, {0.21403688192367554F, 0.21403686702251434F, 0.21403686702251434F}, 4.5e-6},
                    Golden{{2, 2, 2}, {4.953776836395264F, 4.953776836395264F, 4.953776836395264F}, 7.1e-5},
                    Golden{{-0.1F, 0.5F, 1}, {0.11530689895153046F, 0.20904307067394257F, 0.893127977848053F}, 2.1e-5}};
}  // namespace

TEST_CASE("Scene ingress rejects unspecified unknown and unsupported versioned interpretations", "[renderer][scene-color]") {
    const auto policy = GENERATE((SceneColorSourcePolicy{}), (SceneColorSourcePolicy{static_cast<SceneColorEncoding>(255), 1}),
                                 (SceneColorSourcePolicy{SceneColorEncoding::LinearAcesCg, 0}),
                                 (SceneColorSourcePolicy{SceneColorEncoding::LegacyLinearRec709D65, 2}),
                                 (SceneColorSourcePolicy{SceneColorEncoding::SrgbRec709D65, 2}));
    const auto validation = ValidateSceneColorSourcePolicy(policy);
    REQUIRE(validation.HasError());
    CHECK(validation.ErrorValue().code.Value() == SceneColorErrors::InvalidPolicy.code.Value());
    CheckFailure(ConvertSceneColorToAcesCg({1, 1, 1}, policy), SceneColorErrors::InvalidPolicy);
}

TEST_CASE("Canonical AP1 ingress preserves signed HDR and extreme finite components exactly", "[renderer][scene-color]") {
    const auto color = GENERATE((Math::Vec3{-0.25F, 0.18F, 4}),
                                (Math::Vec3{-std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), -0.0F}));
    REQUIRE(ValidateSceneColorSourcePolicy(Canonical).HasValue());
    const auto result = ConvertSceneColorToAcesCg(color, Canonical);
    REQUIRE(result.HasValue());
    CheckColor(result.Value(), color, 0);
}

TEST_CASE("Rec709 basis conversion matches pinned OCIO float rows without transposition", "[renderer][scene-color]") {
    const std::array cases{Golden{{1, 0, 0}, {0.6130974292755127F, 0.07019372284412384F, 0.0206155925989151F}, 0},
                           Golden{{0, 1, 0}, {0.3395231366157532F, 0.9163538813591003F, 0.10956977307796478F}, 0},
                           Golden{{0, 0, 1}, {0.04737945273518562F, 0.013452398590743542F, 0.8698146343231201F}, 0}};
    for (const auto &golden : cases) {
        const auto result = ConvertSceneColorToAcesCg(golden.input, Linear);
        REQUIRE(result.HasValue());
        CheckColor(result.Value(), golden.output, golden.tolerance);
    }
}

TEST_CASE("Linear Rec709 ingress matches independent signed and HDR OCIO observations", "[renderer][scene-color]") {
    for (const auto &golden : LinearGoldens) {
        const auto result = ConvertSceneColorToAcesCg(golden.input, Linear);
        REQUIRE(result.HasValue());
        CheckColor(result.Value(), golden.output, golden.tolerance);
    }
}

TEST_CASE("sRGB ingress matches independent OCIO observations with explicit nonlinear error budgets", "[renderer][scene-color]") {
    for (const auto &golden : SrgbGoldens) {
        const auto result = ConvertSceneColorToAcesCg(golden.input, Srgb);
        REQUIRE(result.HasValue());
        CheckColor(result.Value(), golden.output, golden.tolerance);
    }
}

TEST_CASE("Negative sRGB uses the linear extension independently for every channel", "[renderer][scene-color]") {
    const Math::Vec3 color{-0.5F, -0.01F, -2};
    const auto result = ConvertSceneColorToAcesCg(color, Srgb);
    const auto linear = ConvertSceneColorToAcesCg({color.x / 12.92F, color.y / 12.92F, color.z / 12.92F}, Linear);
    REQUIRE(result.HasValue());
    REQUIRE(linear.HasValue());
    CheckColor(result.Value(), linear.Value(), 1.5e-8);
}

TEST_CASE("Standard sRGB positive HDR and mixed inputs match an independent tight analytic oracle", "[renderer][scene-color]") {
    // Independently calculated using Python double EOTF, pinned float32 matrix coefficients, double dot accumulation
    // and one final float32 rounding. These values do not use OCIO's approximate CPU transfer implementation.
    const std::array cases{Golden{{0.5F, 0.5F, 0.5F}, {0.21404114365577698F, 0.21404114365577698F, 0.21404114365577698F}, 2e-8},
                           Golden{{2, 2, 2}, {4.953845977783203F, 4.953845977783203F, 4.953845977783203F}, 5e-7},
                           Golden{{-0.1F, 0.5F, 1}, {0.11530603468418121F, 0.20904652774333954F, 0.893107533454895F}, 6e-8}};
    for (const auto &golden : cases) {
        const auto result = ConvertSceneColorToAcesCg(golden.input, Srgb);
        REQUIRE(result.HasValue());
        CheckColor(result.Value(), golden.output, golden.tolerance);
    }
}

TEST_CASE("Ingress rejects each nonfinite source component for every supported encoding", "[renderer][scene-color]") {
    const auto policy = GENERATE(Canonical, Linear, Srgb);
    const auto invalid =
        GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
    for (const auto color : std::array{Math::Vec3{invalid, 1, 1}, Math::Vec3{1, invalid, 1}, Math::Vec3{1, 1, invalid}})
        CheckFailure(ConvertSceneColorToAcesCg(color, policy), SceneColorErrors::NonFiniteInput);
}

TEST_CASE("Finite sRGB HDR overflow fails rather than clamping the canonical result", "[renderer][scene-color]") {
    const auto maximum = std::numeric_limits<float>::max();
    for (const auto color : std::array{Math::Vec3{maximum, 0, 0}, Math::Vec3{0, maximum, 0}, Math::Vec3{0, 0, maximum}})
        CheckFailure(ConvertSceneColorToAcesCg(color, Srgb), SceneColorErrors::OutOfRange);
}

TEST_CASE("Linear ingress accumulates finite extreme signed values without float intermediate overflow", "[renderer][scene-color]") {
    const auto maximum = std::numeric_limits<float>::max();
    const auto result = ConvertSceneColorToAcesCg({maximum, -maximum, maximum}, Linear);
    REQUIRE(result.HasValue());
    CHECK(Math::IsFinite(result.Value()));
    CHECK(result.Value().y < 0);
    CHECK(result.Value().z > 0);
}
