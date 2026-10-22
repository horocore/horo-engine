#include "VulkanPresentationTestSupport.h"

namespace Horo::Render::VulkanPresentationTest {
    TEST_CASE("Vulkan legacy SDR does not silently enable attachment transfer encoding", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        CHECK(fixture.state.selectedFormat == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(fixture.state.acquires == 0);
    }

    TEST_CASE("Vulkan rejects incompatible legacy surface encoding before chain creation or acquisition", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        SECTION("Only sRGB attachment") {
            fixture.state.outputFormat.format = VK_FORMAT_B8G8R8A8_SRGB;
        }
        SECTION("Foreign colorspace") {
            fixture.state.outputFormat.colorSpace = VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;
        }
        const auto resized = fixture.runtime.Resize({640, 480}, PresentMode::Fifo);
        REQUIRE(resized.HasError());
        CHECK(resized.ErrorValue().domain.Value() == RenderPresentationErrors::OutputUnsupported.domain.Value());
        CHECK(resized.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
        CHECK(fixture.state.chainsCreated == 0);
        CHECK(fixture.state.acquires == 0);
    }

    TEST_CASE("Vulkan resize rejects a foreign pipeline format while retaining the current chain", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.state.outputFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
        const auto resized = fixture.runtime.Resize({800, 600}, PresentMode::Fifo);
        REQUIRE(resized.HasError());
        CHECK(resized.ErrorValue().domain.Value() == RenderPresentationErrors::OutputUnsupported.domain.Value());
        CHECK(resized.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
        CHECK(fixture.runtime.OutputFormat().Value() == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(fixture.state.chainsCreated == 1);
        CHECK(fixture.state.chainsDestroyed == 0);
        CHECK(fixture.state.acquires == 0);
    }

    TEST_CASE("Vulkan rejected format transition preserves the same pending acquisition debt", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        REQUIRE_FALSE(fixture.runtime.PollAvailability({640, 480}).Value());
        const auto acquired = fixture.state.lastAcquire;
        fixture.state.outputFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
        CHECK(fixture.runtime.Resize({800, 600}, PresentMode::Fifo).HasError());
        CHECK(fixture.state.releases == 0);
        CHECK(fixture.state.chainsDestroyed == 0);
        fixture.state.signaled[acquired] = true;
        REQUIRE(fixture.runtime.PollAvailability({640, 480}).Value());
        REQUIRE(fixture.runtime.BeginFrame({1}).HasValue());
        fixture.runtime.Abort({1});
    }

    TEST_CASE("Vulkan pending acquisition survives shutdown until the exact fence signals", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        REQUIRE_FALSE(fixture.runtime.PollAvailability({640, 480}).Value());
        CHECK(fixture.runtime.BeginFrame({1}).HasError());
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.state.releases == 0);
        CHECK(fixture.state.chainsDestroyed == 0);
        CHECK_FALSE(fixture.state.invalidDestroy);
        fixture.state.signaled[fixture.state.lastAcquire] = true;
        REQUIRE(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.state.released == std::vector<std::uint32_t>{0});
        CHECK(fixture.state.chainsDestroyed == 1);
        CHECK(fixture.state.validAcquire);
        CHECK(fixture.state.blockingLookups == 0);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan resize preserves pending acquisition and explicitly replaces the drained chain", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        REQUIRE_FALSE(fixture.runtime.PollAvailability({640, 480}).Value());
        const auto pending = fixture.runtime.Resize({800, 600}, PresentMode::Fifo);
        REQUIRE(pending.HasError());
        CHECK(pending.ErrorValue().domain.Value() == RenderPresentationErrors::DrawableUnavailable.domain.Value());
        CHECK(pending.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
        CHECK(fixture.state.chainsDestroyed == 0);
        CHECK(fixture.state.releases == 0);
        fixture.state.signaled[fixture.state.lastAcquire] = true;
        REQUIRE(fixture.runtime.Resize({1024, 768}, PresentMode::Fifo).HasValue());
        CHECK(fixture.state.chainsCreated == 2);
        CHECK(fixture.state.chainsDestroyed == 1);
        CHECK(fixture.state.releases == 1);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan unsent frame abort returns the acquired image without GPU completion fiction", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.AcquireReady();
        REQUIRE(fixture.runtime.BeginFrame({1}).HasValue());
        fixture.runtime.Abort({99});
        CHECK(fixture.runtime.Resize({800, 600}, PresentMode::Fifo).HasError());
        CHECK(fixture.state.releases == 0);
        fixture.runtime.Abort({1});
        REQUIRE(fixture.runtime.Resize({800, 600}, PresentMode::Fifo).HasValue());
        CHECK(fixture.state.releases == 1);
        CHECK_FALSE(fixture.state.gpuComplete);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan render completion cannot substitute for independent presentation fence retirement", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.AcquireReady();
        const auto output = fixture.runtime.BeginFrame({1});
        REQUIRE(output.HasValue());
        REQUIRE(fixture.runtime.Submitted({1}, {{1}, 7}).HasValue());
        REQUIRE(fixture.runtime.Present({1}).HasValue());
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Pending);
        fixture.state.gpuComplete = true;
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.state.chainsDestroyed == 0);
        fixture.state.signaled[fixture.state.lastPresent] = true;
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.state.chainsDestroyed == 1);
        CHECK(fixture.state.releases == 0);
        CHECK(fixture.state.validPresent);
        CHECK(fixture.state.blockingLookups == 0);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan suboptimal acquisition completes its fence debt before explicit reconfiguration", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.state.acquireResult = VK_SUBOPTIMAL_KHR;
        fixture.AcquireReady();
        REQUIRE(fixture.runtime.BeginFrame({1}).HasValue());
        fixture.runtime.Abort({1});
        REQUIRE(fixture.runtime.Resize({640, 480}, PresentMode::Fifo).HasValue());
        CHECK(fixture.state.releases == 1);
        CHECK(fixture.state.chainsCreated == 2);
    }

    TEST_CASE("Vulkan out of date acquisition does not invent a signaled fence or image", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.state.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
        CHECK(fixture.runtime.PollAvailability({640, 480}).HasError());
        CHECK(fixture.runtime.BeginFrame({1}).HasError());
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.state.releases == 0);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan failed unenqueued present retains submitted image until command completion", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.AcquireReady();
        REQUIRE(fixture.runtime.BeginFrame({1}).HasValue());
        REQUIRE(fixture.runtime.Submitted({1}, {{1}, 7}).HasValue());
        fixture.state.presentResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        CHECK(fixture.runtime.Present({1}).HasError());
        fixture.runtime.Abort({1});
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.state.releases == 0);
        fixture.state.gpuComplete = true;
        REQUIRE(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.state.releases == 1);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }

    TEST_CASE("Vulkan surface lost present preserves enqueued presentation debt through teardown", "[renderer][vulkan][wsi]") {
        Fixture fixture;
        fixture.Realize();
        fixture.AcquireReady();
        REQUIRE(fixture.runtime.BeginFrame({1}).HasValue());
        REQUIRE(fixture.runtime.Submitted({1}, {{1}, 7}).HasValue());
        fixture.state.presentResult = VK_ERROR_SURFACE_LOST_KHR;
        CHECK(fixture.runtime.Present({1}).HasError());
        REQUIRE(fixture.runtime.BeginShutdown().HasValue());
        fixture.state.gpuComplete = true;
        CHECK(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Pending);
        CHECK(fixture.state.chainsDestroyed == 0);
        fixture.state.signaled[fixture.state.lastPresent] = true;
        REQUIRE(fixture.runtime.PollDrain().Value() == RenderBackendDrainProgress::Drained);
        CHECK(fixture.state.releases == 0);
        CHECK_FALSE(fixture.state.invalidDestroy);
    }
}  // namespace Horo::Render::VulkanPresentationTest
