#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    class TrackingStaticMeshExecutor final : public Horo::Render::IStaticMeshPassExecutor {
    public:
        Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &) override {
            ++executeCount;
            return Horo::Result<void>::Success();
        }

        std::size_t executeCount{0};
    };

    TEST_CASE("Frontend Stages Execution And Presentation", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 50, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();
        Check(lifecycleState.frameActive);
        Check(lifecycleState.executeCount == 0);
        Check(lifecycleState.presentCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        Check(lifecycleState.frameActive);
        Check(lifecycleState.executeCount == 1);
        Check(lifecycleState.presentCount == 0);
        Check(frame.Present().HasValue());
        Check(!lifecycleState.frameActive);
        Check(lifecycleState.presentCount == 1);
        Check(lifecycleState.abortCount == 0);
    }

    TEST_CASE("Frame Scope Move Transfers Abort Ownership", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        {
            auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 51, .outputExtent = {800, 600}});
            Check(begun.HasValue());
            RenderFrameScope original = std::move(begun).Value();
            RenderFrameScope moved = std::move(original);
            Check(lifecycleState.frameActive);
            (void)moved;
        }
        Check(!lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 1);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{2}, .kind = RenderPassKind::Graphics},
        };
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 52, .outputExtent = {800, 600}}, passes).HasValue());
    }

    TEST_CASE("Frame Scope Rejects Invalid Stage Transitions Without Losing Recovery", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 53, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        const Result<void> earlyPresent = frame.Present();
        Check(earlyPresent.HasError());
        Check(earlyPresent.ErrorValue().code.Value() == "render.frontend.frame_not_executed");
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        const Result<void> duplicateExecute = frame.Execute(passes);
        Check(duplicateExecute.HasError());
        Check(duplicateExecute.ErrorValue().code.Value() == "render.frontend.frame_already_executed");
        Check(lifecycleState.executeCount == 1);
        Check(frame.Present().HasValue());
        Check(!lifecycleState.frameActive);
    }

    TEST_CASE("Frontend Rejects A Second Frame Without Aborting The Owned Scope", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 54, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        const auto duplicate = frontend->BeginFrame(FrameDescriptor{.frameNumber = 55, .outputExtent = {800, 600}});
        Check(duplicate.HasError());
        Check(duplicate.ErrorValue().code.Value() == "render.frontend.frame_already_active");
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        Check(frame.Present().HasValue());
    }

    TEST_CASE("Frontend Destruction Aborts And Invalidates An Outstanding Scope", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 56, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        frontend.reset();
        Check(lifecycleState.abortCount == 1);
        Check(lifecycleState.shutdownCount == 1);
        Check(!lifecycleState.frameActive);

        const Result<void> inactive = frame.Present();
        Check(inactive.HasError());
        Check(inactive.ErrorValue().code.Value() == "render.frontend.frame_not_active");
    }

    TEST_CASE("Frontend native timing is explicit and timed present validates exact host identity", "[renderer][pacing][frontend]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const auto unsupported = frontend->PollNativePresentTiming();
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == "render.frame_pacing.native_timing_unsupported");
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {1280, 720}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}}};
        REQUIRE(frame.Execute(passes).HasValue());

        class PresentationClock final : public Clock {
        public:
            Duration MonotonicNow() const override {
                return {};
            }
        } clock;

        const PresentationTimingRequest stale{{1, 1}, 2, clock};
        CHECK(frame.Present(&stale).HasError());
        const PresentationTimingRequest exact{{1, 1}, 1, clock};
        auto moved = std::move(frame);
        REQUIRE(moved.Present(&exact).HasValue());
    }

    TEST_CASE("Frontend Shutdown Leaves Resident Native Resource Cleanup To The Backend",
              "[unit][runtime][renderer][resource][retirement]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 16> bytes{};
        const auto buffer = frontend->CreateBuffer({.byteSize = bytes.size(),
                                                    .usage = RenderBufferUsage::Vertex,
                                                    .access = RenderBufferAccess::DeviceLocal},
                                                   bytes);
        REQUIRE(buffer.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.createBufferCount == 1);
        CHECK(lifecycleState.destroyBufferCount == 0);

        frontend.reset();

        CHECK(lifecycleState.shutdownCount == 1);
        CHECK(lifecycleState.destroyBufferCount == 0);
    }

    TEST_CASE("Frame Scope Move Assignment Aborts The Previous Frame And Transfers Ownership", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> trackingFrontend = CreateTrackingFrontend();
        auto trackingBegun = trackingFrontend->BeginFrame(FrameDescriptor{.frameNumber = 57, .outputExtent = {800, 600}});
        Check(trackingBegun.HasValue());
        RenderFrameScope destination = std::move(trackingBegun).Value();

        RenderBackendRegistry nullRegistry;
        Check(RegisterNullRenderBackend(nullRegistry).HasValue());
        Check(nullRegistry.Seal().HasValue());
        auto nullCreated = RenderFrontend::Create(nullRegistry, RenderBackendId{"null"}, RenderBackendConfig{});
        Check(nullCreated.HasValue());
        std::unique_ptr<RenderFrontend> nullFrontend = std::move(nullCreated).Value();
        auto nullBegun = nullFrontend->BeginFrame(FrameDescriptor{.frameNumber = 58, .outputExtent = {800, 600}});
        Check(nullBegun.HasValue());
        RenderFrameScope source = std::move(nullBegun).Value();

        destination = std::move(source);
        Check(lifecycleState.abortCount == 1);
        Check(!lifecycleState.frameActive);
        const Result<void> movedFrom = source.Present();
        Check(movedFrom.HasError());
        Check(movedFrom.ErrorValue().code.Value() == "render.frontend.frame_not_active");

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{5}, .kind = RenderPassKind::Graphics},
        };
        Check(destination.Execute(passes).HasValue());
        Check(destination.Present().HasValue());
    }

    TEST_CASE("Frame Scope Supports Self Move And Move After Execute", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 59, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        frame = std::move(frame);
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{6}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        RenderFrameScope moved = std::move(frame);
        Check(moved.Present().HasValue());
        Check(lifecycleState.executeCount == 1);
        Check(lifecycleState.presentCount == 1);
        Check(lifecycleState.abortCount == 0);
    }

    TEST_CASE("Frontend Rejects Resize During Active Frame And Supports Explicit Cancel", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 60, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        Check(frontend->Capabilities().backend == RenderBackendId{"tracking"});
        const Result<void> resize = frontend->Resize(FramebufferExtent{1024, 768});
        Check(resize.HasError());
        Check(resize.ErrorValue().code.Value() == "render.frontend.resize_during_frame");
        Check(lifecycleState.resizeCount == 0);
        Check(lifecycleState.frameActive);

        frame.Cancel();
        Check(lifecycleState.abortCount == 1);
        Check(!lifecycleState.frameActive);
        frame.Cancel();
        Check(lifecycleState.abortCount == 1);
        Check(frontend->Resize(FramebufferExtent{1024, 768}).HasValue());
        Check(lifecycleState.resizeCount == 1);
    }

    TEST_CASE("Frontend Owns Static Mesh Executor Attachment And Rejects Missing Execution", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);
        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array staticPass{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .staticMesh = StaticMeshPassDescriptor{}}};
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 50, .outputExtent = {64, 64}});
        Check(begun.HasValue());
        RenderFrameScope missingExecutorFrame = std::move(begun).Value();
        const Result<void> missing = missingExecutorFrame.Execute(staticPass);
        Check(missing.HasError());
        Check(missing.ErrorValue().code.Value() == "render.frontend.static_mesh_executor_missing");

        TrackingStaticMeshExecutor first;
        TrackingStaticMeshExecutor second;
        Check(frontend->AttachStaticMeshPassExecutor(first).HasValue());
        const Result<void> duplicate = frontend->AttachStaticMeshPassExecutor(second);
        Check(duplicate.HasError());
        Check(duplicate.ErrorValue().code.Value() == "render.frontend.static_mesh_executor_already_attached");
        frontend->DetachStaticMeshPassExecutor(first);
        Check(frontend->AttachStaticMeshPassExecutor(second).HasValue());
        frontend->DetachStaticMeshPassExecutor(second);

        const Result<RenderTargetHandle> invalidTarget = frontend->CreateOffscreenTarget({});
        Check(invalidTarget.HasError());
        auto target = frontend->CreateOffscreenTarget({32, 24});
        Check(target.HasValue());
        Check(frontend->ResizeOffscreenTarget(target.Value(), {64, 48}).HasValue());
        const Result<void> invalidResize = frontend->ResizeOffscreenTarget(target.Value(), {});
        Check(invalidResize.HasError());
        Check(invalidResize.ErrorValue().code.Value() == "render.frontend.invalid_target_extent");
        Check(frontend->ReleaseOffscreenTarget(target.Value()).HasValue());
        const Result<void> staleResize = frontend->ResizeOffscreenTarget(target.Value(), {16, 16});
        Check(staleResize.HasError());
        Check(staleResize.ErrorValue().code.Value() == "render.frontend.resource.stale");
        auto replacement = frontend->CreateOffscreenTarget({16, 16});
        Check(replacement.HasValue());
        Check(replacement.Value().owner == target.Value().owner);
        Check(replacement.Value().slot == target.Value().slot);
        Check(replacement.Value().generation == target.Value().generation + 1);
    }
}  // namespace
