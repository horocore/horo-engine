#include "RenderFrontendTestSupport.h"
#include "support/TypedIdentityTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;
}  // namespace

TEST_CASE("Frontend lighting admission requires implemented native predicates and authored fallback", "[renderer][light-preparation]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    const RenderLightPreparationReadiness ready{true, true, true};
    const auto strict =
        frontend->ResolveLightPreparation(LightPreparationMode::ClusteredForwardNative, LightPreparationFallback::Forbidden, ready);
    Tests::RequireActionableError(strict, LightCullingErrors::Unsupported);
    const auto fallback = frontend->ResolveLightPreparation(LightPreparationMode::ClusteredForwardNative,
                                                            LightPreparationFallback::AllowBaselineForwardCpu, ready);
    REQUIRE(fallback.HasValue());
    CHECK(fallback.Value().mode == LightPreparationMode::BaselineForwardCpu);
    CHECK(fallback.Value().missingPredicates == 15);
    CHECK(lifecycleState.executeCount == 0);
    const auto baseline =
        frontend->ResolveLightPreparation(LightPreparationMode::BaselineForwardCpu, LightPreparationFallback::Forbidden, {});
    REQUIRE(baseline.HasValue());
    CHECK(baseline.Value().missingPredicates == 0);
}

TEST_CASE("Frontend lighting selection is a cold operation before the active frame", "[renderer][light-preparation]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
    REQUIRE(begun.HasValue());
    auto frame = std::move(begun).Value();
    const auto rejected =
        frontend->ResolveLightPreparation(LightPreparationMode::BaselineForwardCpu, LightPreparationFallback::Forbidden, {});
    Tests::RequireActionableError(rejected, LightCullingErrors::InvalidInput);
    CHECK(lifecycleState.executeCount == 0);
    frame.Cancel();
    CHECK(frontend->ResolveLightPreparation(LightPreparationMode::BaselineForwardCpu, LightPreparationFallback::Forbidden, {}).HasValue());
}

TEST_CASE("Frontend issues stable view identities only for cold viewport creation", "[renderer][light-preparation][view]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    const auto first = frontend->CreateRenderViewIdentity();
    const auto second = frontend->CreateRenderViewIdentity();
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    CHECK(first.Value().IsValid());
    CHECK(second.Value().value > first.Value().value);
    auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
    REQUIRE(begun.HasValue());
    auto frame = std::move(begun).Value();
    Tests::RequireActionableError(frontend->CreateRenderViewIdentity(), LightCullingErrors::InvalidInput);
    frame.Cancel();
    const auto third = frontend->CreateRenderViewIdentity();
    REQUIRE(third.HasValue());
    CHECK(third.Value().value == second.Value().value + 1);
    CHECK(lifecycleState.executeCount == 0);
}
