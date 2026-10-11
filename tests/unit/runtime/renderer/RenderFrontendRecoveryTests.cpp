#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    TEST_CASE("Frontend Initializes And Owns Selected Backend Lifetime", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        Check(lifecycleState.initializeCount == 1);

        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        Check(frontend->Capabilities().backend == RenderBackendId{"tracking"});
        frontend.reset();

        Check(lifecycleState.shutdownCount == 1);
    }

    TEST_CASE("Frontend Submits Frames And Recovers After Plan Failure", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(RegisterNullRenderBackend(registry).HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array validPasses{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
            RenderPassDescriptor{.id = RenderPassId{2}, .kind = RenderPassKind::Copy},
        };
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {1280, 720}}, validPasses).HasValue());

        const std::array unsupportedPasses{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Compute},
        };
        const Result<void> unsupported =
            frontend->SubmitFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {1280, 720}}, unsupportedPasses);
        Check(unsupported.HasError());
        Check(unsupported.ErrorValue().code.Value() == "render.backend.unsupported_pass_kind");

        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 3, .outputExtent = {1280, 720}}, validPasses).HasValue());
    }

    TEST_CASE("Frontend Recovers After Presentation Failure", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.failPresentation = true;
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };
        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 10, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.test.present_failed");
        Check(lifecycleState.abortCount == 1);

        lifecycleState.failPresentation = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 11, .outputExtent = {640, 480}}, passes).HasValue());
    }

    TEST_CASE("Frontend Contains Initialization Exceptions And Cleans Up Partial State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.throwDuringInitialize = true;
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasError());
        Check(created.ErrorValue().code.Value() == "render.frontend.initialize_exception");
        Check(lifecycleState.initializeCount == 1);
        Check(lifecycleState.shutdownCount == 1);
    }

    TEST_CASE("Frontend Contains Frame Exceptions And Recovers Owned Frame State", "[unit][runtime][renderer]") {
        constexpr std::array throwPoints{FrameThrowPoint::Begin, FrameThrowPoint::Execute, FrameThrowPoint::Present};
        for (const FrameThrowPoint throwPoint : throwPoints) {
            lifecycleState = {};
            lifecycleState.frameThrowPoint = throwPoint;
            RenderBackendRegistry registry;
            RegisterTrackingBackend(registry);

            auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
            Check(created.HasValue());
            std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
            const std::array passes{
                RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
            };

            const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 20, .outputExtent = {640, 480}}, passes);
            Check(failed.HasError());
            Check(failed.ErrorValue().code.Value() == "render.frontend.frame_exception");
            Check(lifecycleState.abortCount == 1);

            lifecycleState.frameThrowPoint = FrameThrowPoint::None;
            Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 21, .outputExtent = {640, 480}}, passes).HasValue());
        }
    }

    TEST_CASE("Frontend Owns Resize Boundary And Contains Backend Exceptions", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        Check(frontend->Resize(FramebufferExtent{1920, 1080}).HasValue());
        Check(lifecycleState.resizeCount == 1);

        lifecycleState.failResize = true;
        const Result<void> typedFailure = frontend->Resize(FramebufferExtent{1600, 900});
        Check(typedFailure.HasError());
        Check(typedFailure.ErrorValue().code.Value() == "render.test.resize_failed");
        Check(typedFailure.ErrorValue().domain.Value() == "render.test");
        Check(typedFailure.ErrorValue().severity == ErrorSeverity::Critical);
        Check(typedFailure.ErrorValue().message == "Injected typed resize failure.");

        lifecycleState.failResize = false;
        lifecycleState.throwDuringResize = true;
        const Result<void> failed = frontend->Resize(FramebufferExtent{1280, 720});
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.frontend.resize_exception");

        lifecycleState.throwDuringResize = false;
        Check(frontend->Resize(FramebufferExtent{1280, 720}).HasValue());
    }

    TEST_CASE("Frontend Rejects Invalid Successful Frame Tokens And Recovers Backend State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.returnInvalidFrameToken = true;
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };

        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 30, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.frontend.invalid_frame_token");
        Check(lifecycleState.abortCount == 1);

        lifecycleState.returnInvalidFrameToken = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 31, .outputExtent = {640, 480}}, passes).HasValue());
    }

    TEST_CASE("Frontend Preserves Typed Begin Failures And Recovers Partial Backend State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.failBeginAfterActivation = true;
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };

        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 40, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.test.begin_failed");
        Check(failed.ErrorValue().domain.Value() == "render.test");
        Check(failed.ErrorValue().severity == ErrorSeverity::Error);
        Check(lifecycleState.abortCount == 1);

        lifecycleState.failBeginAfterActivation = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 41, .outputExtent = {640, 480}}, passes).HasValue());
    }

}  // namespace
