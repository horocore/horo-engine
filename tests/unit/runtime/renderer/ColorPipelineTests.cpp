#include "ColorPipelineTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;
}  // namespace

TEST_CASE("Creative look admission owns copied finite RGB data without claiming output conformance", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto descriptor = fixture.Descriptor();
    auto cooked = CookedColorTransform::Create(descriptor, fixture.rgb);
    REQUIRE(cooked.HasValue());
    fixture.rgb[0] = 7;
    CHECK(cooked.Value()->Rgb()[0] == 0);
    CHECK(cooked.Value()->Rgb().size() == 24);
    CHECK(cooked.Value()->Descriptor().payloadDigest == descriptor.payloadDigest);
}

TEST_CASE("Cooked color rejects corrupt payloads nonfinite entries and incomplete cubes", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto descriptor = fixture.Descriptor();
    SECTION("digest mismatch") {
        fixture.rgb[0] = 7;
    }
    SECTION("nan even with matching hash") {
        fixture.rgb[0] = std::numeric_limits<float>::quiet_NaN();
        descriptor.payloadDigest = ColorPayloadDigest(fixture.rgb);
    }
    SECTION("partial cube") {
        auto result = CookedColorTransform::Create(descriptor, std::span{fixture.rgb}.first(23));
        REQUIRE(result.HasError());
        return;
    }
    REQUIRE(CookedColorTransform::Create(descriptor, fixture.rgb).HasError());
}

TEST_CASE("Cooked color requires pinned provenance and closed schema interpolation and role", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto descriptor = fixture.Descriptor();
    SECTION("configuration") {
        descriptor.configurationDigest = {};
    }
    SECTION("reference corpus") {
        descriptor.referenceDigest = {};
    }
    SECTION("release") {
        descriptor.configurationRelease = 0;
    }
    SECTION("schema") {
        descriptor.schemaVersion = 3;
    }
    SECTION("dimension") {
        descriptor.dimension = 66;
    }
    SECTION("interpolation") {
        descriptor.interpolation = static_cast<ColorLutInterpolation>(255);
    }
    SECTION("role") {
        descriptor.role = static_cast<ColorTransformRole>(255);
    }
    REQUIRE(CookedColorTransform::Create(descriptor, fixture.rgb).HasError());
}

TEST_CASE("Color candidate pins immutable transforms and does not mutate source request or active state", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    auto plan = PrepareColorPipeline(request);
    REQUIRE(plan.HasValue());
    const auto transform = request.outputTransform;
    request.outputTransform.reset();
    request.generation = 999;
    CHECK(plan.Value().Inputs().generation == fixture.source.compatibility.colorGeneration);
    CHECK(plan.Value().Inputs().outputTransform == transform);
}

TEST_CASE("Color admission rejects encoded low precision or foreign scene input", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    SECTION("display encoded") {
        request.sceneColor.representation = PostProcessRepresentation::TransferEncoded;
    }
    SECTION("RGBA8") {
        request.sceneColor.descriptor.format = RenderTextureFormat::Rgba8Unorm;
    }
    SECTION("view") {
        ++request.sceneColor.compatibility.view.value;
    }
    SECTION("exposure generation") {
        ++request.sceneColor.compatibility.exposureGeneration;
    }
    SECTION("color generation") {
        ++request.generation;
    }
    SECTION("not scene color") {
        request.sceneColor.semantic = PostProcessSemantic::Normal;
    }
    REQUIRE(PrepareColorPipeline(request).HasError());
}

TEST_CASE("Color output requires the exact admitted native encoding without neutral fallback", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    SECTION("missing") {
        request.outputTransform.reset();
    }
    SECTION("foreign primaries") {
        request.output.active.colorSpace = RenderDisplayColorSpace::DisplayP3;
    }
    SECTION("foreign transfer") {
        request.output.active.transfer = RenderDisplayTransferFunction::Linear;
    }
    SECTION("HDR") {
        request.output.active.dynamicRange = RenderDisplayDynamicRange::High;
    }
    SECTION("different format") {
        request.output.active.format = RenderTextureFormat::Rgba16Float;
    }
    SECTION("missing native revision") {
        request.output.revision = 0;
    }
    SECTION("unknown dither") {
        request.dither = static_cast<ColorDitherPolicy>(255);
    }
    REQUIRE(PrepareColorPipeline(request).HasError());
}

TEST_CASE("Color candidate capacity failure and cancellation leave previous candidate usable", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    auto prior = PrepareColorPipeline(request);
    REQUIRE(prior.HasValue());
    request.maximumTransformBytes = 95;
    auto denied = PrepareColorPipeline(request);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == ColorPipelineErrors::CapacityExceeded.code.Value());
    CancellationSource cancellation;
    request = fixture.Request();
    request.cancellation = cancellation.Token();
    cancellation.RequestCancellation();
    denied = PrepareColorPipeline(request);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == ColorPipelineErrors::Cancelled.code.Value());
    CHECK(prior.Value().Inputs().outputTransform->TableUpload(1).size() == 5792);
}

TEST_CASE("Color plan charges complete immutable program and creative look payloads at the exact budget", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    const auto look = CookedColorTransform::Create(fixture.Descriptor(), fixture.rgb);
    REQUIRE(look.HasValue());
    request.look = look.Value();
    request.maximumTransformBytes = request.outputTransform->PayloadBytes() + look.Value()->PayloadBytes();
    const auto admitted = PrepareColorPipeline(request);
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().Inputs().look == request.look);
    --request.maximumTransformBytes;
    const auto denied = PrepareColorPipeline(request);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == ColorPipelineErrors::CapacityExceeded.code.Value());
    CHECK(admitted.Value().Inputs().outputTransform->Source() == request.outputTransform->Source());
}

TEST_CASE("Creative look cannot attach a foreign configuration to the closed output program", "[renderer][color]") {
    ColorPipelineFixture fixture;
    auto request = fixture.Request();
    auto descriptor = fixture.Descriptor();
    descriptor.configurationDigest.bytes[0] ^= 1;
    const auto look = CookedColorTransform::Create(descriptor, fixture.rgb);
    REQUIRE(look.HasValue());
    request.look = look.Value();
    REQUIRE(PrepareColorPipeline(request).HasError());
}
