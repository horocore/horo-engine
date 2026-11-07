#include "ColorOutputShaderRecipe.h"
#include "ColorPipelineTestSupport.h"

#include <array>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;

    /** @brief Reads one exact portable upload float; no runtime sampling algorithm is duplicated by these tests. */
    float UploadFloat(std::span<const std::byte> bytes, std::size_t index) {
        const auto offset = index * 4;
        REQUIRE(offset + 4 <= bytes.size());
        std::uint32_t bits{};
        for (unsigned i = 0; i < 4; ++i)
            bits |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
        return std::bit_cast<float>(bits);
    }

    /** @brief Creates copied non-native metadata data for cold admission, never a native interpolation or ACES oracle. */
    std::shared_ptr<const CookedColorTransform> Look(ColorPipelineFixture &fixture, ColorLutInterpolation interpolation) {
        auto descriptor = fixture.Descriptor();
        descriptor.interpolation = interpolation;
        auto value = CookedColorTransform::Create(descriptor, fixture.rgb);
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }
}  // namespace

TEST_CASE("Creative atlas preserves original RGB order exactly and pads only alpha", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    const auto look = Look(fixture, ColorLutInterpolation::Tetrahedral);
    const auto descriptor = look->AtlasDescriptor();
    CHECK(descriptor.extent.width == 4);
    CHECK(descriptor.extent.height == 2);
    CHECK(descriptor.format == RenderTextureFormat::Rgba32Float);
    CHECK(descriptor.usage == (RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination));
    REQUIRE(look->AtlasUpload().size() == 8 * 16);
    for (std::size_t cell = 0; cell < 8; ++cell) {
        for (std::size_t channel = 0; channel < 3; ++channel)
            CHECK(UploadFloat(look->AtlasUpload(), cell * 4 + channel) == fixture.rgb[cell * 3 + channel]);
        CHECK(UploadFloat(look->AtlasUpload(), cell * 4 + 3) == 1.0F);
    }
    CHECK(look->AtlasDigest() == ComputeSha256(look->AtlasUpload()));
    CHECK(look->PayloadBytes() == fixture.rgb.size() * sizeof(float) + look->AtlasUpload().size());
    fixture.rgb[0] = 99;
    CHECK(UploadFloat(look->AtlasUpload(), 0) == 0);
}

TEST_CASE("Creative atlas bounds retain full dimension65 without implicit tiling or downsampling", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto descriptor = fixture.Descriptor();
    descriptor.dimension = 65;
    const std::vector<float> rgb(65U * 65U * 65U * 3U, .25F);
    descriptor.payloadDigest = ColorPayloadDigest(rgb);
    const auto look = CookedColorTransform::Create(descriptor, rgb);
    REQUIRE(look.HasValue());
    CHECK(look.Value()->AtlasDescriptor().extent.width == 4225);
    CHECK(look.Value()->AtlasDescriptor().extent.height == 65);
    CHECK(look.Value()->AtlasUpload().size() == 65U * 65U * 65U * 16U);
}

TEST_CASE("Executable creative look requires explicit schema2 finite domain and versioned policy", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto descriptor = fixture.Descriptor();
    SECTION("old inert schema has no implicit execution migration") {
        descriptor.schemaVersion = 1;
    }
    SECTION("unknown policy") {
        descriptor.domainPolicy = static_cast<ColorLookDomainPolicy>(255);
    }
    SECTION("empty domain") {
        descriptor.domainMaximum.x = descriptor.domainMinimum.x;
    }
    SECTION("nonfinite domain") {
        descriptor.domainMinimum.y = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("finite endpoints with unrepresentable width") {
        descriptor.domainMinimum.z = -std::numeric_limits<float>::max();
        descriptor.domainMaximum.z = std::numeric_limits<float>::max();
    }
    const auto result = CookedColorTransform::Create(descriptor, fixture.rgb);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == ColorPipelineErrors::InvalidTransform.code.Value());
}

TEST_CASE("Creative negative HDR domains and both declared interpolation modes remain exact metadata", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    for (const auto mode : std::array{ColorLutInterpolation::Trilinear, ColorLutInterpolation::Tetrahedral}) {
        auto descriptor = fixture.Descriptor();
        descriptor.interpolation = mode;
        descriptor.domainMinimum = {-1, -1, -1};
        descriptor.domainMaximum = {3, 3, 3};
        const auto result = CookedColorTransform::Create(descriptor, fixture.rgb);
        REQUIRE(result.HasValue());
        CHECK(result.Value()->Descriptor().interpolation == mode);
        CHECK(result.Value()->Descriptor().domainMinimum.x == -1);
        CHECK(result.Value()->Descriptor().domainMaximum.x == 3);
    }
}

TEST_CASE("Creative mode distinguishes intentional bypass from an enabled missing asset", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    const auto disabled = PrepareColorPipeline(request);
    REQUIRE(disabled.HasValue());
    CHECK(disabled.Value().Inputs().lookMode == ColorLookMode::Disabled);
    request.lookMode = ColorLookMode::Enabled;
    const auto missing = PrepareColorPipeline(request);
    REQUIRE(missing.HasError());
    CHECK(missing.ErrorValue().code.Value() == ColorPipelineErrors::InvalidTransform.code.Value());
    request.look = Look(fixture, ColorLutInterpolation::Trilinear);
    const auto enabled = PrepareColorPipeline(request);
    REQUIRE(enabled.HasValue());
    CHECK(enabled.Value().Inputs().look == request.look);
    request.look.reset();
    CHECK(enabled.Value().Inputs().look->Descriptor().interpolation == ColorLutInterpolation::Trilinear);
    CHECK(PrepareColorPipeline(request).HasError());
    CHECK(disabled.Value().Inputs().look == nullptr);
}

TEST_CASE("Creative enabled plan rejects foreign configuration and unknown mode without fallback", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    request.lookMode = ColorLookMode::Enabled;
    auto descriptor = fixture.Descriptor();
    descriptor.configurationDigest.bytes[0] ^= 1;
    auto look = CookedColorTransform::Create(descriptor, fixture.rgb);
    REQUIRE(look.HasValue());
    request.look = look.Value();
    CHECK(PrepareColorPipeline(request).HasError());
    request = fixture.Request();
    request.lookMode = static_cast<ColorLookMode>(255);
    CHECK(PrepareColorPipeline(request).HasError());
}

TEST_CASE("Valid nondefault numeric grading returns explicit unsupported rather than being ignored", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    constexpr std::array fields{&ColorGradingSettings::temperatureKelvin, &ColorGradingSettings::tint, &ColorGradingSettings::saturation,
                                &ColorGradingSettings::contrast, &ColorGradingSettings::gamma};
    for (const auto field : fields) {
        auto request = fixture.Request();
        request.grading.*field += .25F;
        const auto denied = PrepareColorPipeline(request);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == ColorPipelineErrors::UnsupportedGrading.code.Value());
    }
    constexpr std::array bands{&ColorGradingSettings::shadows, &ColorGradingSettings::midtones, &ColorGradingSettings::highlights};
    for (const auto band : bands) {
        auto request = fixture.Request();
        (request.grading.*band).x = 1.125F;
        const auto denied = PrepareColorPipeline(request);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == ColorPipelineErrors::UnsupportedGrading.code.Value());
    }
}

TEST_CASE("Creative recipe keeps the baseline interface and adds only the independent atlas and float registers",
          "[renderer][color][look]") {
    const ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 16, 128, false, true};
    const auto baseline = MakeColorOutputShaderManifest(ColorOutputShaderStage::SceneToDisplayLinear, target);
    const auto looked = MakeColorOutputShaderManifest(ColorOutputShaderStage::SceneToDisplayLinearWithLook, target);
    REQUIRE(baseline.HasValue());
    REQUIRE(looked.HasValue());
    CHECK(baseline.Value().sourceRevision == 1);
    CHECK(looked.Value().sourceRevision == 2);
    REQUIRE(looked.Value().bindings.size() == baseline.Value().bindings.size() + 1);
    CHECK(looked.Value().bindings.back().id.value == 8);
    CHECK(looked.Value().bindings.back().kind == ShaderResourceKind::SampledTexture);
    CHECK(looked.Value().parameters.size() == baseline.Value().parameters.size() + 3);
    const auto names = ColorOutputShaderReflectionNames(ColorOutputShaderStage::SceneToDisplayLinearWithLook);
    REQUIRE(names[0].parameters.size() == 6);
    CHECK(names[0].parameters[3].name == "LookDomainMinimum");
    CHECK(names[0].parameters[4].name == "LookDomainMaximum");
    CHECK(names[0].parameters[5].name == "LookControl");
    CHECK(names.back().name == "LookCube");
}

TEST_CASE("Cold color selection pins exact assets without requiring invented frame identities", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto analytic = fixture.Request().outputTransform;
    auto look = Look(fixture, ColorLutInterpolation::Trilinear);
    const auto enabled = PrepareColorTransformSelection(analytic, ColorLookMode::Enabled, look);
    REQUIRE(enabled.HasValue());
    CHECK(enabled.Value().Analytic() == analytic);
    CHECK(enabled.Value().Look() == look);
    CHECK(enabled.Value().Mode() == ColorLookMode::Enabled);
    const auto disabled = PrepareColorTransformSelection(analytic, ColorLookMode::Disabled, look);
    REQUIRE(disabled.HasValue());
    CHECK(disabled.Value().Look() == look);
    CHECK(disabled.Value().Mode() == ColorLookMode::Disabled);
    const std::weak_ptr<const CookedColorTransform> retained = look;
    look.reset();
    analytic.reset();
    CHECK_FALSE(retained.expired());
    CHECK(enabled.Value().Analytic() != nullptr);
}

TEST_CASE("Cold color selection rejects required missing and incompatible assets with canonical typed outcomes",
          "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    SECTION("required missing asset") {
        request.lookMode = ColorLookMode::Enabled;
    }
    SECTION("missing analytic generation") {
        request.outputTransform.reset();
    }
    SECTION("unknown look mode") {
        request.lookMode = static_cast<ColorLookMode>(255);
    }
    SECTION("foreign configuration retained while disabled") {
        auto descriptor = fixture.Descriptor();
        descriptor.configurationRelease += 1;
        const auto look = CookedColorTransform::Create(descriptor, fixture.rgb);
        REQUIRE(look.HasValue());
        request.look = look.Value();
    }
    const auto cold = PrepareColorTransformSelection(request.outputTransform, request.lookMode, request.look);
    const auto complete = PrepareColorPipeline(request);
    REQUIRE(cold.HasError());
    REQUIRE(complete.HasError());
    CHECK(cold.ErrorValue().code.Value() == ColorPipelineErrors::InvalidTransform.code.Value());
    CHECK(complete.ErrorValue().code.Value() == cold.ErrorValue().code.Value());
}

TEST_CASE("Cold and complete color admission share grading and aggregate atlas budget validation", "[renderer][color][look]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    request.look = Look(fixture, ColorLutInterpolation::Tetrahedral);
    SECTION("complete original and atlas payload fits exactly") {
        request.maximumTransformBytes = request.outputTransform->PayloadBytes() + request.look->PayloadBytes();
        const auto cold = PrepareColorTransformSelection(request.outputTransform, request.lookMode, request.look, request.grading,
                                                         request.maximumTransformBytes);
        REQUIRE(cold.HasValue());
        CHECK(PrepareColorPipeline(request).HasValue());
    }
    SECTION("one byte under complete storage denies both") {
        request.maximumTransformBytes = request.outputTransform->PayloadBytes() + request.look->PayloadBytes() - 1;
        const auto cold = PrepareColorTransformSelection(request.outputTransform, request.lookMode, request.look, request.grading,
                                                         request.maximumTransformBytes);
        const auto complete = PrepareColorPipeline(request);
        REQUIRE(cold.HasError());
        REQUIRE(complete.HasError());
        CHECK(cold.ErrorValue().code.Value() == ColorPipelineErrors::CapacityExceeded.code.Value());
        CHECK(complete.ErrorValue().code.Value() == cold.ErrorValue().code.Value());
    }
    SECTION("nondefault grading has no cold bypass") {
        request.grading.saturation = 1.25F;
        const auto cold = PrepareColorTransformSelection(request.outputTransform, request.lookMode, request.look, request.grading);
        const auto complete = PrepareColorPipeline(request);
        REQUIRE(cold.HasError());
        REQUIRE(complete.HasError());
        CHECK(cold.ErrorValue().code.Value() == ColorPipelineErrors::UnsupportedGrading.code.Value());
        CHECK(complete.ErrorValue().code.Value() == cold.ErrorValue().code.Value());
    }
}
