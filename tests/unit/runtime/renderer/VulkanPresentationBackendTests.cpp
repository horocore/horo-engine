#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "VulkanBackendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanBackendTest;

    class PresentationPort final : public FakeRuntimePort, public IVulkanCommandPort {
    public:
        explicit PresentationPort(RuntimeState &state) : FakeRuntimePort(state) {}

        bool SupportsWindowPresentation() const noexcept override {
            return true;
        }

        Result<void> ResizePresentation(FramebufferExtent extent, PresentMode mode) override {
            ++resizes;
            output = extent;
            policy = mode;
            return Result<void>::Success();
        }

        Result<bool> PollPresentationAvailability(FramebufferExtent extent) override {
            ++polls;
            return Result<bool>::Success(extent == output && available);
        }

        Result<RenderPresentationOutput> ObservePresentationOutput() const override {
            if (!output.IsValid())
                return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
            return Result<RenderPresentationOutput>::Success({.display = {.revision = 1, .attached = true}, .revision = 1});
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &) override {
            return Result<FrameToken>::Success({1});
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &) override {
            return Result<void>::Success();
        }

        Result<void> SubmitFrame(FrameToken) override {
            ++submitted;
            accepted = selectedSignal;
            if (failedPresentation)
                return Result<void>::Failure(MakeError(RenderPresentationErrors::DrawableUnavailable));
            return Result<void>::Success();
        }

        std::optional<bool> ObserveGraphSubmissionAcceptance(RenderTimelinePoint point, std::uint64_t) const noexcept override {
            return accepted.IsValid() && point == accepted;
        }

        void AbortFrame(FrameToken) noexcept override {
            ++aborted;
        }

        Result<bool> ObserveSubmission(const RenderCpuWait &) override {
            return Result<bool>::Success(true);
        }

        Result<void> BeginShutdownCommands() override {
            return Result<void>::Success();
        }

        Result<RenderBackendDrainProgress> PollCommandDrain() override {
            return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
        }

        FramebufferExtent output;
        PresentMode policy{};
        int resizes{}, polls{}, submitted{}, aborted{};
        bool available{};
        bool failedPresentation{};
        RenderTimelinePoint selectedSignal, accepted;
    };
}  // namespace

TEST_CASE("Vulkan backend delegates realized output and nonblocking acquisition to its one command owner", "[renderer][vulkan][wsi]") {
    auto state = DefaultState();
    PresentationPort port{state};
    auto backend = CreateBackend(port);
    REQUIRE(backend->Initialize({.requirePresentation = true, .presentMode = PresentMode::Fifo}).HasValue());
    CHECK(backend->Capabilities().presentsToWindow);
    const auto unrealized = backend->PresentationOutput();
    REQUIRE(unrealized.HasError());
    CHECK(unrealized.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnavailable.code.Value());
    REQUIRE(backend->Resize({640, 480}).HasValue());
    REQUIRE(backend->PresentationOutput().HasValue());
    CHECK(backend->PresentationOutput().Value().revision == 1);
    CHECK(port.policy == PresentMode::Fifo);
    REQUIRE_FALSE(backend->PollPresentationAvailability({640, 480}).Value());
    port.available = true;
    REQUIRE(backend->PollPresentationAvailability({640, 480}).Value());
    const auto frame = backend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}});
    REQUIRE(frame.HasValue());
    CHECK(backend->Resize({800, 600}).HasError());
    CHECK(backend->PollPresentationAvailability({640, 480}).HasError());
    CHECK(port.resizes == 1);
    CHECK(port.polls == 2);
    REQUIRE(backend->Present(frame.Value()).HasValue());
    CHECK(port.submitted == 1);
    REQUIRE(backend->Resize({800, 600}).HasValue());
    Test::RequireDrainedOwner(*backend);
}

TEST_CASE("Vulkan backend aborts an unsuccessful window tail without losing accepted graph evidence", "[renderer][vulkan][wsi]") {
    auto state = DefaultState();
    PresentationPort port{state};
    auto backend = CreateBackend(port);
    REQUIRE(backend->Initialize({.requirePresentation = true, .presentMode = PresentMode::Fifo}).HasValue());
    REQUIRE(backend->Resize({640, 480}).HasValue());
    port.available = true;
    REQUIRE(backend->PollPresentationAvailability({640, 480}).Value());
    const auto frame = backend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}});
    REQUIRE(frame.HasValue());
    port.selectedSignal = {{1}, 7};
    port.failedPresentation = true;
    const auto presented = backend->Present(frame.Value());
    REQUIRE(presented.HasError());
    CHECK(presented.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
    CHECK(port.aborted == 1);
    const auto device = backend->Capabilities().support.deviceIncarnation;
    CHECK(backend->ObserveGraphSubmissionAcceptance({{1}, 7}, device) == std::optional{true});
    CHECK(backend->ObserveGraphSubmissionAcceptance({{1}, 6}, device) == std::optional{false});
    backend->AbortFrame(frame.Value());
    CHECK(port.aborted == 1);
    Test::RequireDrainedOwner(*backend);
}

TEST_CASE("Vulkan headless command owner does not turn interactive runtime support into selected presentation", "[renderer][vulkan][wsi]") {
    auto state = DefaultState();
    PresentationPort port{state};
    auto backend = CreateBackend(port);
    REQUIRE(backend->Initialize({}).HasValue());
    CHECK_FALSE(backend->Capabilities().presentsToWindow);
    const auto unsupported = backend->PresentationOutput();
    REQUIRE(unsupported.HasError());
    CHECK(unsupported.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
    CHECK(backend->Resize({640, 480}).HasError());
    CHECK(backend->PollPresentationAvailability({640, 480}).Value());
    CHECK(port.resizes == 0);
    CHECK(port.polls == 0);
    Test::RequireDrainedOwner(*backend);
}
