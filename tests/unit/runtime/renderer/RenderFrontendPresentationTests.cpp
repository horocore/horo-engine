#include "RenderFrontendTestSupport.h"

#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    template <typename T> void RequireCode(const Result<T> &result, std::string_view code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }
}  // namespace

TEST_CASE("Presentation readiness and output preparation preserve frontend frame ownership", "[renderer][presentation]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    REQUIRE(frontend->PollPresentationAvailability({64, 64}).Value());
    RequireCode(frontend->ConfigurePresentationOutput({}), "render.presentation.output_unsupported");
    RequireCode(frontend->PresentationOutput(), "render.presentation.output_unsupported");
    {
        auto begun = frontend->BeginFrame({1, {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        RequireCode(frontend->PollPresentationAvailability({64, 64}), "render.frontend.frame_already_active");
        RequireCode(frontend->ConfigurePresentationOutput({}), "render.frontend.resource.change_during_frame");
        CHECK(lifecycleState.frameActive);
    }
    REQUIRE(frontend->BeginShutdown().HasValue());
    RequireCode(frontend->PollPresentationAvailability({64, 64}), "render.frontend.closing");
    RequireCode(frontend->ConfigurePresentationOutput({}), "render.frontend.closing");
    RequireCode(frontend->PresentationOutput(), "render.frontend.closing");
    REQUIRE(frontend->PollDrain().Value() == RenderBackendDrainProgress::Drained);
}

TEST_CASE("Output observations and acquisition admission reject foreign threads before backend access", "[renderer][presentation]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    std::array<std::string, 3> codes;
    std::thread foreign{[&] {
        const auto ready = frontend->PollPresentationAvailability({64, 64});
        const auto configured = frontend->ConfigurePresentationOutput({});
        const auto output = frontend->PresentationOutput();
        codes[0] = ready.HasError() ? ready.ErrorValue().code.Value() : "unexpected.success";
        codes[1] = configured.HasError() ? configured.ErrorValue().code.Value() : "unexpected.success";
        codes[2] = output.HasError() ? output.ErrorValue().code.Value() : "unexpected.success";
    }};
    foreign.join();
    for (const auto &code : codes)
        CHECK(code == "render.frontend.wrong_thread");
    CHECK_FALSE(lifecycleState.frameActive);
    CHECK(lifecycleState.executeCount == 0);
}
