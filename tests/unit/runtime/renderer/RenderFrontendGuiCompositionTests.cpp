#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    class TrackingGuiExecutor final : public IGuiPassExecutor {
    public:
        Result<GuiCompositionPreparation> PrepareComposition(const FrameToken frame) override {
            return Result<GuiCompositionPreparation>::Success(
                {stalePreparation ? FrameToken{frame.value + 1} : frame,
                 explicitSubmission ? GuiCompositionMode::ExplicitGraphicsSubmission : GuiCompositionMode::BorrowedActiveRecording,
                 resources, buffers});
        }

        Result<void> RenderDrawData(const GuiCompositionRequest &) override {
            ++calls;
            CHECK(lifecycleState.frameActive);
            CHECK(lifecycleState.executeCount == 1);
            CHECK(lifecycleState.presentCount == 0);
            if (throws)
                throw std::runtime_error("GUI encoder failure");
            if (fails)
                return Result<void>::Failure(Error{.code = ErrorCode{"test.gui.encoding"},
                                                   .domain = ErrorDomainId{"test.gui"},
                                                   .message = "Injected GUI encoding failure"});
            return Result<void>::Success();
        }

        int calls{0};
        bool fails{false};
        bool throws{false};
        bool stalePreparation{};
        bool explicitSubmission{};
        std::span<const GuiSampledResource> resources;
        std::span<const RenderBufferHandle> buffers;
    };

    TEST_CASE("GUI composition is ordered once and survives frame moves", "[unit][runtime][renderer][gui]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        TrackingGuiExecutor gui;
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        CHECK(frame.ComposeGui(gui).HasError());
        CHECK(gui.calls == 0);
        const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .primaryOutput = PrimaryOutputAttachment{}}};
        REQUIRE(frame.Execute(passes).HasValue());
        REQUIRE(frame.ComposeGui(gui).HasValue());
        auto moved = std::move(frame);
        CHECK(frame.ComposeGui(gui).HasError());
        const auto duplicate = moved.ComposeGui(gui);
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == "render.frontend.gui_already_composed");
        CHECK(gui.calls == 1);
        REQUIRE(moved.Present().HasValue());
        CHECK(moved.ComposeGui(gui).HasError());
        CHECK(lifecycleState.presentCount == 1);
        CHECK(lifecycleState.abortCount == 0);
    }

    TEST_CASE("GUI composition failure cancellation and exceptions abort before presentation", "[unit][runtime][renderer][gui]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        TrackingGuiExecutor gui;
        CancellationSource cancellation;
        SECTION("typed encoder failure") {
            gui.fails = true;
        }
        SECTION("encoder exception") {
            gui.throws = true;
        }
        SECTION("cancelled before encoding") {
            cancellation.RequestCancellation();
        }
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .primaryOutput = PrimaryOutputAttachment{}}};
        REQUIRE(frame.Execute(passes).HasValue());
        const auto composed = frame.ComposeGui(gui, cancellation.Token());
        REQUIRE(composed.HasError());
        CHECK(composed.ErrorValue().code.Value() == (gui.fails    ? "test.gui.encoding"
                                                     : gui.throws ? "render.frontend.frame_exception"
                                                                  : "render.frontend.gui_cancelled"));
        CHECK(gui.calls == (cancellation.Token().IsCancellationRequested() ? 0 : 1));
        CHECK(frame.Present().HasError());
        CHECK(lifecycleState.abortCount == 1);
        CHECK(lifecycleState.presentCount == 0);
        REQUIRE(frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 64}}).HasValue());
    }

    TEST_CASE("GUI explicit tail uses the same frontend reservation history and completion lease", "[renderer][gui][submission]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        const auto abandoned = frame.ReserveSubmission(RenderQueueRole::Graphics);
        REQUIRE(abandoned.HasValue());
        TrackingGuiExecutor gui;
        gui.explicitSubmission = true;
        REQUIRE(frame.ComposeGui(gui).HasValue());
        CHECK(gui.calls == 1);
        CHECK(lifecycleState.guiOrder.value == abandoned.Value().order.value + 1);
        CHECK(lifecycleState.guiSignal.value == abandoned.Value().signal.value + 1);
        REQUIRE(lifecycleState.graphLease != nullptr);
        const std::array dependency{lifecycleState.guiSignal};
        CHECK(frame.ReserveSubmission(RenderQueueRole::Graphics, dependency).HasValue());
        REQUIRE(frame.Present().HasValue());
        CHECK(lifecycleState.graphLease == nullptr);
    }

    TEST_CASE("GUI stale declarations and rejected native admission never encode or commit a frame", "[renderer][gui][submission]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        TrackingGuiExecutor gui;
        SECTION("stale frame") {
            gui.stalePreparation = true;
        }
        SECTION("native rejects transferred lease") {
            gui.explicitSubmission = true;
            lifecycleState.failGuiAdmission = true;
        }
        REQUIRE(frame.ComposeGui(gui).HasError());
        CHECK(gui.calls == 0);
        CHECK(lifecycleState.abortCount == 1);
        CHECK(lifecycleState.graphLease == nullptr);
        CHECK(frame.Present().HasError());
    }

    TEST_CASE("GUI sampled view generations retain frontend charges until actual delayed completion", "[renderer][gui][resource]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const auto texture =
            frontend->CreateTexture({.extent = {2, 2}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled});
        REQUIRE(texture.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        const auto view = frontend->CreateTextureView(
            {.texture = texture.Value().handle, .format = RenderTextureFormat::Rgba8Unorm, .aspect = RenderTextureAspect::Color});
        REQUIRE(view.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        TrackingGuiExecutor gui;
        const std::array samples{GuiSampledResource{view.Value().handle, {}}};
        gui.resources = samples;
        lifecycleState.completeGraphOnPresent = false;
        REQUIRE(frame.ComposeGui(gui).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTextureView(view.Value().handle).HasValue());
        REQUIRE(frontend->ReleaseTexture(texture.Value().handle).HasValue());
        CHECK(lifecycleState.destroyTextureViewCount == 0);
        CHECK(lifecycleState.destroyTextureCount == 0);
        REQUIRE(lifecycleState.graphLease != nullptr);
        lifecycleState.graphLease->Release();
        lifecycleState.graphLease = nullptr;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.destroyTextureViewCount == 1);
        CHECK(lifecycleState.destroyTextureCount == 1);
    }

    TEST_CASE("GUI foreign sampled generation is rejected before native retention or drawing", "[renderer][gui][resource]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        TrackingGuiExecutor gui;
        const std::array samples{GuiSampledResource{RenderTextureViewHandle{{999}, 1, 1}, {}}};
        gui.resources = samples;
        CHECK(frame.ComposeGui(gui).HasError());
        CHECK(lifecycleState.guiAdmissions == 0);
        CHECK(gui.calls == 0);
        CHECK(lifecycleState.abortCount == 1);
    }

    TEST_CASE("GUI exact encoder buffer pins survive logical release until completion", "[renderer][gui][buffer]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const std::array<std::byte, 16> bytes{};
        const auto buffer = frontend->CreateBuffer({16, RenderBufferUsage::Vertex, RenderBufferAccess::HostVisible}, bytes);
        REQUIRE(buffer.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        TrackingGuiExecutor gui;
        gui.explicitSubmission = true;
        const std::array buffers{buffer.Value().handle};
        gui.buffers = buffers;
        lifecycleState.completeGraphOnPresent = false;
        REQUIRE(frame.ComposeGui(gui).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseBuffer(buffer.Value().handle).HasValue());
        CHECK(lifecycleState.destroyBufferCount == 0);
        REQUIRE(lifecycleState.graphLease != nullptr);
        lifecycleState.graphLease->Release();
        lifecycleState.graphLease = nullptr;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.destroyBufferCount == 1);
    }

    TEST_CASE("GUI foreign encoder buffers fail before backend admission", "[renderer][gui][buffer]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.Execute({}).HasValue());
        TrackingGuiExecutor gui;
        gui.explicitSubmission = true;
        const std::array buffers{RenderBufferHandle{{999}, 1, 1}};
        gui.buffers = buffers;
        CHECK(frame.ComposeGui(gui).HasError());
        CHECK(lifecycleState.guiAdmissions == 0);
        CHECK(gui.calls == 0);
    }
}  // namespace
