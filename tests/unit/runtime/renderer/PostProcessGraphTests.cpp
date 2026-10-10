#include "PostProcessTestSupport.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <limits>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;
}  // namespace

TEST_CASE("Post-process graphs preserve generations and keep visibility independent from color", "[runtime][renderer][post-process]") {
    PostProcessFixture fixture;
    fixture.EnableAll();
    auto plan = PreparePostProcessGraph(fixture.Request());
    REQUIRE(plan.HasValue());
    const auto &value = plan.Value();
    REQUIRE(value.Graph().has_value());
    REQUIRE(value.Passes().size() == 8);
    REQUIRE(value.Graph()->Dependencies().size() == 6);
    REQUIRE(value.AmbientVisibility().has_value());
    CHECK(value.SceneColor() != *value.AmbientVisibility());
    CHECK(value.Compatibility() == fixture.compatibility);
    CHECK(value.SettingsGeneration() == 7);
    auto schedule = CompileRenderGraph(*value.Graph());
    REQUIRE(schedule.HasValue());
    CHECK(schedule.Value().OrderedPasses().size() == 8);
    const auto &ao = value.Passes()[0];
    CHECK(ao.representation == PostProcessRepresentation::Data);
    CHECK(std::ranges::none_of(value.Graph()->Dependencies(), [&](const auto &d) {
        return d.before == ao.pass || d.after == ao.pass;
    }));
    for (const auto &pass : value.Passes().subspan(1))
        CHECK(pass.representation == PostProcessRepresentation::UnexposedAcesCg);
    fixture.settings.bloom->intensity = 9;
    fixture.inputs.clear();
    fixture.recipes.clear();
    CHECK(value.Settings().bloom->intensity == 1);
    CHECK(value.Input(PostProcessSemantic::LinearDepth).IsValid());
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<PostProcessGraphPlan>);
}

TEST_CASE("Disabled effects create an explicit no-work pass-through and AO alone exports a lighting input",
          "[runtime][renderer][post-process]") {
    PostProcessFixture fixture;
    auto empty = PreparePostProcessGraph(fixture.Request());
    REQUIRE(empty.HasValue());
    CHECK_FALSE(empty.Value().Graph().has_value());
    CHECK(empty.Value().Passes().empty());
    CHECK(empty.Value().SourceSceneColor() == fixture.inputs[0].texture);
    fixture.settings.ambientOcclusion.emplace();
    auto ao = PreparePostProcessGraph(fixture.Request());
    REQUIRE(ao.HasValue());
    CHECK(ao.Value().Passes().size() == 1);
    CHECK(ao.Value().Graph()->Dependencies().empty());
    CHECK(ao.Value().SceneColor() == ao.Value().Input(PostProcessSemantic::SceneColor));
}

TEST_CASE("Post-process graph rejects missing required semantic inputs and exposure", "[runtime][renderer][post-process]") {
    const std::size_t removed = GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U);
    PostProcessFixture fixture;
    fixture.EnableAll();
    auto request = fixture.Request();
    if (removed == 6)
        request.exposure.reset();
    else {
        fixture.inputs.erase(fixture.inputs.begin() + static_cast<std::ptrdiff_t>(removed));
        request.inputs = fixture.inputs;
    }
    auto denied = PreparePostProcessGraph(request);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == PostProcessErrors::MissingInput.code.Value());
}

TEST_CASE("Post-process graph rejects stale incompatible and aliased inputs without fallback", "[runtime][renderer][post-process]") {
    const int variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    PostProcessFixture fixture;
    fixture.EnableAll();
    if (variant == 0)
        ++fixture.inputs[0].compatibility.exposureGeneration;
    if (variant == 1)
        ++fixture.inputs[5].compatibility.colorGeneration;
    if (variant == 2)
        fixture.inputs[0].representation = PostProcessRepresentation::DisplayLinear;
    if (variant == 3)
        fixture.inputs[0].descriptor.format = RenderTextureFormat::Rgba8Unorm;
    if (variant == 4)
        fixture.inputs[1].representation = PostProcessRepresentation::UnexposedAcesCg;
    if (variant == 5)
        fixture.inputs[5].texture = fixture.inputs[0].texture;
    if (variant == 6)
        ++fixture.inputs[5].compatibility.view.value;
    if (variant == 7)
        ++fixture.inputs[5].compatibility.recipeGeneration;
    auto request = fixture.Request();
    if (variant == 8)
        ++request.exposure->generation;
    auto denied = PreparePostProcessGraph(request);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == PostProcessErrors::IncompatibleInput.code.Value());
}

TEST_CASE("Post-process recipes choose only explicit preferences and enforce cumulative budgets", "[runtime][renderer][post-process]") {
    PostProcessFixture fixture;
    fixture.settings.bloom.emplace();
    auto compute = fixture.recipes[4];
    compute.cookedVariant = 99;
    compute.passKind = RenderPassKind::Compute;
    fixture.recipes.insert(fixture.recipes.begin(), compute);
    fixture.capabilities.queues.compute = false;
    auto request = fixture.Request();
    auto selected = PreparePostProcessGraph(request);
    REQUIRE(selected.HasValue());
    CHECK(selected.Value().Passes()[0].recipe.cookedVariant == 5);
    request.recipes = std::span{fixture.recipes}.first(1);
    CHECK(PreparePostProcessGraph(request).ErrorValue().code.Value() == PostProcessErrors::UnsupportedRecipe.code.Value());
    request = fixture.Request();
    request.maximumReservedBytes = 4095;
    CHECK(PreparePostProcessGraph(request).ErrorValue().code.Value() == PostProcessErrors::UnsupportedRecipe.code.Value());
    request = fixture.Request();
    request.maximumWorkItems = 2559;
    CHECK(PreparePostProcessGraph(request).ErrorValue().code.Value() == PostProcessErrors::UnsupportedRecipe.code.Value());
    fixture.settings.vignette.emplace();
    request = fixture.Request();
    request.maximumReservedBytes = 4096;
    CHECK(PreparePostProcessGraph(request).ErrorValue().code.Value() == PostProcessErrors::UnsupportedRecipe.code.Value());
}

TEST_CASE("Post-process candidate rejection leaves an existing immutable generation usable", "[runtime][renderer][post-process]") {
    PostProcessFixture fixture;
    fixture.settings.vignette.emplace();
    auto prior = PreparePostProcessGraph(fixture.Request());
    REQUIRE(prior.HasValue());
    auto request = fixture.Request();
    request.order[0] = request.order[1];
    REQUIRE(PreparePostProcessGraph(request).HasError());
    fixture.settings.vignette->intensity = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(PreparePostProcessGraph(fixture.Request()).HasError());
    auto moved = std::move(prior).Value();
    CHECK(moved.SettingsGeneration() == 7);
    REQUIRE(CompileRenderGraph(*moved.Graph()).HasValue());
}

TEST_CASE("Post-process settings reject nonfinite and unbounded authored quality values", "[runtime][renderer][post-process]") {
    const int variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    PostProcessFixture fixture;
    fixture.EnableAll();
    if (variant == 0)
        fixture.settings.bloom->downsampleLevels = 17;
    if (variant == 1)
        fixture.settings.depthOfField->apertureFStop = 0;
    if (variant == 2)
        fixture.settings.motionBlur->sampleCount = 257;
    if (variant == 3)
        fixture.settings.ambientOcclusion->mode = static_cast<AmbientOcclusionMode>(255);
    if (variant == 4)
        fixture.settings.reflections->thickness = -1;
    if (variant == 5)
        fixture.settings.chromaticAberration->intensityPixels = std::numeric_limits<float>::infinity();
    if (variant == 6)
        fixture.settings.filmGrain->scale = 0;
    if (variant == 7)
        fixture.settings.colorGrading.gamma = 0;
    if (variant == 8)
        fixture.settings.colorGrading.shadows.x = std::numeric_limits<float>::quiet_NaN();
    if (variant == 9)
        fixture.settings.exposureCompensationEv = 33;
    REQUIRE(ValidatePostProcessSettings(fixture.settings).HasError());
}
