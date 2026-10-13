#include "Horo/Runtime/Render/RenderShaderResourceErrors.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo::Render;

TEST_CASE("Graphics pipeline state identity covers every immutable raster and output dimension", "[render][pipeline][descriptor]") {
    const RenderGraphicsPipelineState state;
    const auto baseline = ComputeRenderGraphicsPipelineStateDigest(state);
    REQUIRE(baseline.HasValue());
    std::array<RenderGraphicsPipelineState, 9> changes;
    changes.fill(state);
    changes[0].cull = RenderCullMode::None;
    changes[1].frontFace = RenderFrontFace::Clockwise;
    changes[2].blend = RenderBlendMode::Alpha;
    changes[3].depthTest = false;
    changes[4].depthWrite = false;
    changes[5].depthCompare = RenderCompareFunction::Greater;
    changes[6].colorFormat = RenderTextureFormat::Rgba16Float;
    changes[7].depthFormat = RenderTextureFormat::Depth32Float;
    changes[8].sampleCount = 4;
    for (const auto &changed : changes) {
        const auto digest = ComputeRenderGraphicsPipelineStateDigest(changed);
        REQUIRE(digest.HasValue());
        CHECK(digest.Value() != baseline.Value());
    }
}

TEST_CASE("Graphics pipeline state rejects unknown enums and incompatible output contracts", "[render][pipeline][descriptor]") {
    RenderGraphicsPipelineState state;
    SECTION("Unknown cull") {
        state.cull = static_cast<RenderCullMode>(255);
    }
    SECTION("Unknown front face") {
        state.frontFace = static_cast<RenderFrontFace>(255);
    }
    SECTION("Unknown blend") {
        state.blend = static_cast<RenderBlendMode>(255);
    }
    SECTION("Unknown depth comparison") {
        state.depthCompare = static_cast<RenderCompareFunction>(255);
    }
    SECTION("Depth format as color output") {
        state.colorFormat = RenderTextureFormat::Depth24Stencil8;
    }
    SECTION("Color format as depth output") {
        state.depthFormat = RenderTextureFormat::Rgba8Unorm;
    }
    SECTION("Depth state without depth output") {
        state.depthFormat.reset();
    }
    SECTION("Non-power-of-two sampling") {
        state.sampleCount = 3;
    }
    Horo::Tests::RequireError(ComputeRenderGraphicsPipelineStateDigest(state), RenderShaderResourceErrors::InvalidPipeline);
}
