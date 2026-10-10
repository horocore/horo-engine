#include "OpenGLRenderTestSupport.h"
#include "OpenGLResourceGraphTestSupport.h"
#include "RenderTransientGraphTestSupport.h"

namespace Horo::Render::OpenGLResourceTests {
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
