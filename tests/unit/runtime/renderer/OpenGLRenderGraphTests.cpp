#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "OpenGLRenderTestSupport.h"
#include "OpenGLResourceGraphTestSupport.h"
#include "RenderTransientGraphTestSupport.h"

#include <limits>

namespace Horo::Render::OpenGLResourceTests {
    /** @brief Compiles one canonical transient depth producer; real backend preparation owns its FBO and completion lease. */
    TransientTest::GraphSources DepthGraph(const RenderTextureFormat format = RenderTextureFormat::Depth32Float) {
        auto builder = Test::RequireBuilder();
        const auto texture = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Texture));
        const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        Test::RequireUsage(builder, {pass, texture, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
        const RenderTextureDescriptor descriptor{.extent = {2, 2}, .format = format, .usage = RenderTextureUsage::RenderAttachment};
        const std::array requirements{RenderGraphTransientRequirement{texture, descriptor}};
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        return TransientTest::CompileSources(builder, requirements, {}, queues, {{pass, RenderGraphDepthAttachment{texture, 0.375F}}});
    }

    TEST_CASE("OpenGL scene-linear half-float graph color uses an exact cold-prepared color attachment", "[unit][renderer][opengl][vfx]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        auto builder = Test::RequireBuilder();
        const auto texture = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Texture));
        const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        Test::RequireUsage(builder, {pass, texture, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        const RenderTextureDescriptor descriptor{.extent = {2, 2},
                                                 .format = RenderTextureFormat::Rgba16Float,
                                                 .usage = RenderTextureUsage::RenderAttachment};
        const std::array requirements{RenderGraphTransientRequirement{texture, descriptor}};
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const PrimaryOutputAttachment clear{.clearColor = {0.25F, 0.5F, 0.75F, 1}};
        auto sources =
            TransientTest::CompileSources(builder, requirements, {}, queues, {{pass, RenderGraphColorAttachment{texture, clear}}});
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(resourceCommandState.imageInternalFormat == 0x881A);   // GL_RGBA16F
        CHECK(resourceCommandState.attachmentPoints[0] == 0x8CE0U);  // GL_COLOR_ATTACHMENT0
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        CHECK(OpenGLBackendTests::commandState.clearCount == 1);
        CHECK(OpenGLBackendTests::commandState.clearedColors[0].red == 0.25F);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
    }

    TEST_CASE("OpenGL graph depth clear uses the prepared depth attachment and restores caller state",
              "[unit][renderer][opengl][depth][vfx]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        auto sources = DepthGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        REQUIRE(resourceCommandState.attachments == 1);
        CHECK(resourceCommandState.attachmentPoints[0] == 0x8D00U);  // GL_DEPTH_ATTACHMENT
        const auto callerFramebuffer = OpenGLBackendTests::commandState.framebuffer;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        CHECK(graphCommandState.depthClears == 1);
        CHECK(graphCommandState.depthValue == 0.375F);
        CHECK(graphCommandState.depthFramebuffer != 0);
        CHECK(graphCommandState.depthFramebuffer != callerFramebuffer);
        CHECK(OpenGLBackendTests::commandState.framebuffer == callerFramebuffer);
        CHECK(OpenGLBackendTests::commandState.clearCount == 0);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(resourceCommandState.deletedTextures == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        {
            const auto drain = TransientTest::BeginFrame(*frontend, 2);
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(resourceCommandState.deletedTextures == 1);
        CHECK(resourceCommandState.deletedFramebuffers == 1);
    }

    TEST_CASE("OpenGL failed depth encoding retains its native attachment through canonical abort completion",
              "[unit][renderer][opengl][depth][vfx]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        auto sources = DepthGraph(RenderTextureFormat::Depth24Stencil8);
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(resourceCommandState.attachmentPoints[0] == 0x821AU);  // GL_DEPTH_STENCIL_ATTACHMENT
        graphCommandState.failDepthClear = true;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasError());
        CHECK(graphCommandState.depthClears == 1);
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(resourceCommandState.deletedTextures == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        {
            const auto drain = TransientTest::BeginFrame(*frontend, 2);
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(resourceCommandState.deletedTextures == 1);
    }

    TEST_CASE("OpenGL nonfinite depth clear is rejected before native encoding or completion transfer",
              "[unit][renderer][opengl][depth][vfx]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        auto sources = DepthGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        std::get<RenderGraphDepthAttachment>(sources.workloads[0].workload).clearDepth = std::numeric_limits<float>::quiet_NaN();
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;  // Keep the conservative abort fence pending.
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasError());
        CHECK(graphCommandState.depthClears == 0);
        CHECK(OpenGLBackendTests::commandState.clearCount == 0);
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);  // Frame abort fences conservatively even before any encoded workload.
        frame.Cancel();
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(resourceCommandState.deletedTextures == 1);  // No backend completion lease was transferred for rejected work.
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
    }

    TEST_CASE("OpenGL transient native copies reuse one object and await the actual fence", "[renderer][opengl][transient]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        const auto buffers = TransientTest::ImportedCopyBuffers(*frontend);
        auto sources = TransientTest::CopyGraph(buffers[0], buffers[1]);
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(resourceCommandState.generatedBuffers == 3);
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;  // GL_TIMEOUT_EXPIRED
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(graphCommandState.copyCount == 4);
        CHECK(graphCommandState.copies[0][1] == graphCommandState.copies[1][0]);
        CHECK(graphCommandState.copies[0][1] == graphCommandState.copies[2][1]);
        CHECK(graphCommandState.sourceBinding == 81);
        CHECK(graphCommandState.destinationBinding == 82);
        auto busy = TransientTest::BeginFrame(*frontend, 2);
        REQUIRE(busy.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasError());
        CHECK(graphCommandState.copyCount == 4);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;  // GL_ALREADY_SIGNALED
        auto completed = TransientTest::BeginFrame(*frontend, 3);
        REQUIRE(completed.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(completed.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(resourceCommandState.deletedBuffers == 0);
        {
            const auto drain = TransientTest::BeginFrame(*frontend, 4);
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(resourceCommandState.deletedBuffers == 1);
    }

    TEST_CASE("OpenGL partial native graph failure retains backing through fence failure and shutdown", "[renderer][opengl][transient]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        const auto buffers = TransientTest::ImportedCopyBuffers(*frontend);
        auto sources = TransientTest::CopyGraph(buffers[0], buffers[1]);
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        graphCommandState.failCopy = true;
        OpenGLBackendTests::commandState.fenceFails = true;
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasError());
        CHECK(graphCommandState.copyCount == 1);
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(resourceCommandState.deletedBuffers == 0);
        const auto failedClose = frontend->BeginShutdown();
        REQUIRE(failedClose.HasError());
        CHECK(failedClose.ErrorValue().code.Value() == "render.opengl.synchronization_failed");
        const auto pending = frontend->PollDrain();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        CHECK(resourceCommandState.deletedBuffers == 0);
        OpenGLBackendTests::commandState.fenceFails = false;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        REQUIRE(frontend->BeginShutdown().HasValue());
        const auto closing = frontend->PollDrain();
        REQUIRE(closing.HasValue());
        CHECK(closing.Value() == RenderBackendDrainProgress::Pending);
        CHECK(resourceCommandState.deletedBuffers == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        frontend.reset();
        CHECK(resourceCommandState.deletedBuffers == 3);
        CHECK(OpenGLBackendTests::commandState.fenceCount >= 1);
    }

    TEST_CASE("OpenGL transient color attachment is created once and native framebuffer retirement follows completion",
              "[renderer][opengl][transient]") {
        ResourcePresentationPort port;
        auto frontend = CreateGraphFrontend(port);
        auto sources = TransientTest::ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(resourceCommandState.generatedTextures == 1);
        CHECK(resourceCommandState.generatedFramebuffers == 1);
        auto frame = TransientTest::BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(resourceCommandState.deletedTextures == 0);
        {
            const auto drain = TransientTest::BeginFrame(*frontend, 2);
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(resourceCommandState.deletedTextures == 1);
        CHECK(resourceCommandState.deletedFramebuffers == 1);
    }
}  // namespace Horo::Render::OpenGLResourceTests
