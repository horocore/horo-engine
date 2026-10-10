#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderGraphInspectionTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Owns a sealed headless composition and begins actual frame scopes for inspection tests. */
    struct FrontendFixture {
        FrontendFixture() {
            const auto registered = RegisterNullRenderBackend(registry);
            REQUIRE(registered.HasValue());
            const auto sealed = registry.Seal();
            REQUIRE(sealed.HasValue());
            auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, RenderBackendConfig{});
            REQUIRE(created.HasValue());
            frontend = std::move(created).Value();
        }

        RenderFrameScope Begin(const std::uint64_t frameNumber) {
            auto begun = frontend->BeginFrame({.frameNumber = frameNumber, .outputExtent = {640, 360}});
            REQUIRE(begun.HasValue());
            return std::move(begun).Value();
        }

        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;
    };
}  // namespace

TEST_CASE("Frontend graph inspection uses real frame identity and never turns cancelled inspection into a rendering failure",
          "[renderer][inspection][frontend]") {
    FrontendFixture fixture;
    auto &frontend = fixture.frontend;
    REQUIRE(frontend->GraphInspectionSnapshot().HasValue());
    CHECK_FALSE(frontend->GraphInspectionSnapshot().Value());
    const auto sources = Test::CompileSources();
    auto scope = fixture.Begin(17);
    const auto captured = scope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution);
    REQUIRE(captured.HasValue());
    CHECK(captured.Value()->Context().renderer.IsValid());
    CHECK(captured.Value()->Context().frame.IsValid());
    CHECK(captured.Value()->Context().revision == 1);
    CHECK(frontend->GraphInspectionSnapshot().Value() == captured.Value());
    std::stop_source stopped;
    stopped.request_stop();
    Test::RequireError(scope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution, {},
                                               stopped.get_token()),
                       "render.graph.inspection.cancelled");
    CHECK(frontend->GraphInspectionSnapshot().Value() == captured.Value());
    const std::array passes{RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics}};
    REQUIRE(scope.Execute(passes).HasValue());
    Test::RequireError(scope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution),
                       "render.graph.inspection.invalid_source");
    REQUIRE(scope.Present().HasValue());
    frontend.reset();
    CHECK(captured.Value()->Passes().size() == 3);
    CHECK(ExportRenderGraphInspection(*captured.Value()).HasValue());
}

TEST_CASE("Frontend replacement rejects old frame scopes and retains no previous-generation inspection publication",
          "[renderer][inspection][frontend][replacement]") {
    FrontendFixture fixture;
    auto &frontend = fixture.frontend;
    const auto sources = Test::CompileSources();
    auto scope = fixture.Begin(1);
    const auto captured = scope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution);
    REQUIRE(captured.HasValue());
    const auto owner = captured.Value()->Context().renderer;
    frontend.reset();
    Test::RequireError(scope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution),
                       "render.graph.inspection.invalid_source");
    FrontendFixture replacementFixture;
    auto &replacement = replacementFixture.frontend;
    CHECK_FALSE(replacement->GraphInspectionSnapshot().Value());
    auto nextScope = replacementFixture.Begin(1);
    const auto changed = nextScope.CaptureInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution);
    REQUIRE(changed.HasValue());
    CHECK(changed.Value()->Context().renderer != owner);
    CHECK(changed.Value()->Context().revision == 1);
    CHECK(captured.Value()->Context().renderer == owner);
}
