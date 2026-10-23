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

        Result<FrameToken> BeginFrame(const FrameDescriptor &) override {
            return Result<FrameToken>::Success({1});
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &) override {
            return Result<void>::Success();
        }

        Result<void> SubmitFrame(FrameToken) override {
            ++submitted;
            return Result<void>::Success();
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
    };
}  // namespace

TEST_CASE("Vulkan backend delegates realized output and nonblocking acquisition to its one command owner", "[renderer][vulkan][wsi]") {
    auto state = DefaultState();
    PresentationPort port{state};
    auto backend = CreateBackend(port);
    REQUIRE(backend->Initialize({.requirePresentation = true, .presentMode = PresentMode::Fifo}).HasValue());
    CHECK(backend->Capabilities().presentsToWindow);
    REQUIRE(backend->Resize({640, 480}).HasValue());
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

TEST_CASE("Vulkan headless command owner does not turn interactive runtime support into selected presentation", "[renderer][vulkan][wsi]") {
    auto state = DefaultState();
    PresentationPort port{state};
    auto backend = CreateBackend(port);
    REQUIRE(backend->Initialize({}).HasValue());
    CHECK_FALSE(backend->Capabilities().presentsToWindow);
    CHECK(backend->Resize({640, 480}).HasError());
    CHECK(backend->PollPresentationAvailability({640, 480}).Value());
    CHECK(port.resizes == 0);
    CHECK(port.polls == 0);
    Test::RequireDrainedOwner(*backend);
}
