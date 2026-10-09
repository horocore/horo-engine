#include "OpenGLRenderTestSupport.h"

#include <limits>
#include <thread>

namespace Horo::Render::OpenGLBackendTests {
    namespace {
        struct CommandFixture {
            PortState portState;
            FakePresentationPort port{portState};
            std::unique_ptr<IRenderBackend> backend;

            explicit CommandFixture(const std::uint32_t slots = 2) {
                commandState = {};
                backend = CreateBackend(port);
                Check(backend->Initialize(RenderBackendConfig{.maxFramesInFlight = slots}).HasValue());
            }

            FrameToken Begin() {
                const auto begun = backend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}});
                Check(begun.HasValue());
                return begun.Value();
            }
        };

        void CheckRestoredState(const CommandState &before) {
            Check(commandState.viewport == before.viewport);
            Check(commandState.secondaryViewport == before.secondaryViewport);
            Check(commandState.secondaryScissorEnabled == before.secondaryScissorEnabled);
            Check(commandState.colorMask == before.colorMask);
            Check(commandState.framebuffer == before.framebuffer);
            Check(commandState.defaultDrawBuffers == before.defaultDrawBuffers);
            Check(commandState.secondaryColorMask == before.secondaryColorMask);
            Check(commandState.enabled == before.enabled);
            Check(commandState.color.red == before.color.red);
            Check(commandState.color.green == before.color.green);
            Check(commandState.color.blue == before.color.blue);
            Check(commandState.color.alpha == before.color.alpha);
        }
    }  // namespace

    TEST_CASE("OpenGL Commands Isolate Primary Output State And Preserve Ordered Clears", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        commandState.color = {0.8F, 0.7F, 0.6F, 0.5F};
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        CheckRestoredState(before);
        const std::array passes{
            BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {0.1F, 0.2F, 0.3F, 1.0F}),
            BackendTestSupport::MakeClearGraphicsPass(RenderPassId{2}, {0.4F, 0.5F, 0.6F, 1.0F}),
        };
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 2);
        Check(commandState.clearedColors[0].red == 0.1F);
        Check(commandState.clearedColors[1].red == 0.4F);
        Check(commandState.clearedFramebuffer == 0);
        const std::array<float, 4> viewport{0, 0, 640, 480};
        const std::array<std::uint8_t, 4> mask{1, 1, 1, 1};
        Check(commandState.clearedViewport == viewport);
        Check(commandState.clearedMask == mask);
        const std::array<bool, 4> disabled{false, false, false, false};
        Check(commandState.clearedEnabled == disabled);
        CheckRestoredState(before);
    }

    TEST_CASE("OpenGL Clears Execute Despite Inherited Rasterizer Discard And Restore It", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        SECTION("Inherited discard enabled") {
            commandState.enabled[3] = true;
        }
        SECTION("Inherited discard disabled") {
            commandState.enabled[3] = false;
        }
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {0.25F, 0.5F, 0.75F, 1.0F})};
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 1);
        Check(!commandState.clearedEnabled[3]);
        Check(commandState.clearedColors[0].red == 0.25F);
        CheckRestoredState(before);
    }

    TEST_CASE("OpenGL Restores Indexed Output Selections And Preserves Higher Color Masks", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        commandState.defaultDrawBuffers = {0x0400, 0x0402};  // FRONT_LEFT, BACK_LEFT
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {0.25F, 0.5F, 0.75F, 1.0F})};
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 1);
        Check(commandState.clearedDrawBuffers[0] == 0x0405);  // BACK
        Check(commandState.clearedDrawBuffers[1] == 0);
        const std::array<std::uint8_t, 4> unmasked{1, 1, 1, 1};
        Check(commandState.clearedMask == unmasked);
        Check(commandState.clearedSecondaryMask == before.secondaryColorMask);
        CheckRestoredState(before);
    }

    TEST_CASE("OpenGL Rejects Draw Buffer Counts Outside Fixed Isolation Storage", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        commandState.maxDrawBuffers = 65;
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {})};
        const auto executed = fixture.backend->Execute({.frame = frame, .orderedPasses = passes});
        Check(executed.HasError());
        Check(executed.ErrorValue().code.Value() == "render.opengl.command_failed");
        Check(commandState.clearCount == 0);
        CheckRestoredState(before);
    }

    TEST_CASE("OpenGL Restores Fractional Viewport And Preserves Higher Indexed Viewport And Scissor",
              "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        commandState.viewport = {0.25F, 1.5F, 80.75F, 90.125F};
        SECTION("Higher scissor enabled") {
            commandState.secondaryScissorEnabled = true;
        }
        SECTION("Higher scissor disabled") {
            commandState.secondaryScissorEnabled = false;
        }
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {})};
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 1);
        Check(!commandState.clearedEnabled[0]);
        Check(commandState.clearedSecondaryViewport == before.secondaryViewport);
        Check(commandState.clearedSecondaryScissorEnabled == before.secondaryScissorEnabled);
        CheckRestoredState(before);
    }

    TEST_CASE("OpenGL Load And DontCare Attachments Do Not Clear Retained Contents", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1},
                                 .primaryOutput = PrimaryOutputAttachment{.loadOperation = AttachmentLoadOperation::Load,
                                                                          .storeOperation = AttachmentStoreOperation::Store}},
            RenderPassDescriptor{.id = RenderPassId{2},
                                 .primaryOutput = PrimaryOutputAttachment{.loadOperation = AttachmentLoadOperation::DontCare,
                                                                          .storeOperation = AttachmentStoreOperation::DontCare}},
            RenderPassDescriptor{.id = RenderPassId{3}},
        };
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 0);
    }

    TEST_CASE("OpenGL Preserves Frontend StaticMesh Executor Ownership", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        // The frontend validates and encodes this workload before submitting its ordered metadata.
        const std::array passes{RenderPassDescriptor{.id = RenderPassId{1}, .staticMesh = StaticMeshPassDescriptor{}}};
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearCount == 0);
        Check(fixture.backend->Present(frame).HasValue());
        Check(commandState.fenceCount == 1);
    }

    TEST_CASE("OpenGL Validates The Entire Plan Before Native Commands", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        auto second = BackendTestSupport::MakeClearGraphicsPass(RenderPassId{2}, {});
        SECTION("Compute workload") {
            second.kind = RenderPassKind::Compute;
        }
        SECTION("Copy workload") {
            second.kind = RenderPassKind::Copy;
        }
        SECTION("Duplicate identity") {
            second.id = RenderPassId{1};
        }
        SECTION("Invalid identity") {
            second.id = {};
        }
        SECTION("Non-finite clear") {
            second.primaryOutput->clearColor.red = std::numeric_limits<float>::infinity();
        }
        SECTION("Invalid load") {
            second.primaryOutput->loadOperation = static_cast<AttachmentLoadOperation>(255);
        }
        SECTION("Invalid store") {
            second.primaryOutput->storeOperation = static_cast<AttachmentStoreOperation>(255);
        }
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {}), second};
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = passes}).HasError());
        Check(commandState.clearCount == 0);
        Check(commandState.viewportCount == 0);
    }

    TEST_CASE("OpenGL Bounds Ordered Plan Work Before Native Commands", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        std::array<RenderPassDescriptor, 1025> passes;
        for (std::size_t index = 0; index < passes.size(); ++index)
            passes[index].id = RenderPassId{static_cast<std::uint32_t>(index + 1)};
        const auto oversized = fixture.backend->Execute({.frame = frame, .orderedPasses = passes});
        Check(oversized.HasError());
        Check(oversized.ErrorValue().code.Value() == "render.opengl.work_limit");
        Check(commandState.viewportCount == 0);
        Check(fixture.backend->Execute({.frame = frame, .orderedPasses = std::span{passes}.first(1024)}).HasValue());
    }

    TEST_CASE("OpenGL Restores State On Typed Driver Failures", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const CommandState before = commandState;
        const FrameToken frame = fixture.Begin();
        SECTION("Driver error before encoding") {
            commandState.error = 0x0502;
        }
        SECTION("Driver error preparing output") {
            commandState.errorOnPrepare = true;
        }
        SECTION("Driver error during encoding") {
            commandState.errorOnClear = true;
        }
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {})};
        const auto executed = fixture.backend->Execute({.frame = frame, .orderedPasses = passes});
        Check(executed.HasError());
        Check(executed.ErrorValue().code.Value() == "render.opengl.command_failed");
        CheckRestoredState(before);
        if (commandState.errorOnPrepare)
            Check(commandState.clearCount == 0);
        fixture.backend->AbortFrame(frame);
        Check(fixture.backend->BeginFrame({.frameNumber = 2, .outputExtent = {640, 480}}).HasValue());
    }

    TEST_CASE("OpenGL Frame Admission Polls Without Waiting And Enforces InFlight Budget", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture{1};
        commandState.pollStatus = 0x911B;  // GL_TIMEOUT_EXPIRED
        Check(fixture.backend->Present(fixture.Begin()).HasValue());
        Check(commandState.fenceCount == 1);
        Check(commandState.flushCount == 1);
        const auto full = fixture.backend->BeginFrame({.frameNumber = 2, .outputExtent = {640, 480}});
        Check(full.HasError());
        Check(full.ErrorValue().code.Value() == "render.opengl.frame_backpressure");
        Check(commandState.pollCount == 1);
        Check(commandState.deleteFenceCount == 0);
        commandState.pollStatus = 0x911C;  // GL_CONDITION_SATISFIED
        Check(fixture.backend->BeginFrame({.frameNumber = 2, .outputExtent = {640, 480}}).HasValue());
        Check(commandState.deleteFenceCount == 1);
    }

    TEST_CASE("OpenGL Abort Retires Host Work And Shutdown Releases Pending Fences", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture{1};
        commandState.pollStatus = 0x911B;
        const FrameToken frame = fixture.Begin();
        fixture.backend->AbortFrame(FrameToken{frame.value + 1});
        Check(commandState.fenceCount == 0);
        fixture.backend->AbortFrame(frame);
        fixture.backend->AbortActiveFrame();
        Check(commandState.fenceCount == 1);
        Check(fixture.backend->BeginFrame({.frameNumber = 2, .outputExtent = {640, 480}}).HasError());
        fixture.backend->Shutdown();
        fixture.backend->Shutdown();
        Check(commandState.deleteFenceCount == 1);
        Check(fixture.portState.destroyCount == 1);
        Check(fixture.backend->Initialize({}).HasValue());
        Check(fixture.backend->BeginFrame({.frameNumber = 3, .outputExtent = {640, 480}}).HasValue());
    }

    TEST_CASE("OpenGL Synchronization Failures Are Typed And Prevent Unbounded Admission", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture{1};
        const FrameToken frame = fixture.Begin();
        SECTION("Fence creation failure") {
            commandState.fenceFails = true;
            const auto presented = fixture.backend->Present(frame);
            Check(presented.HasError());
            Check(presented.ErrorValue().code.Value() == "render.opengl.synchronization_failed");
            Check(fixture.portState.swapCount == 0);
            fixture.backend->AbortFrame(frame);
        }
        SECTION("Fence poll failure") {
            Check(fixture.backend->Present(frame).HasValue());
            commandState.pollStatus = 0x911D;  // GL_WAIT_FAILED
        }
        const auto failed = fixture.backend->BeginFrame({.frameNumber = 2, .outputExtent = {640, 480}});
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.opengl.synchronization_failed");
    }

    TEST_CASE("OpenGL Swap Retry Replaces Fence And Preserves Active Token", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        fixture.portState.failure = PortFailure::Swap;
        Check(fixture.backend->Present(frame).HasError());
        fixture.portState.failure = PortFailure::None;
        Check(fixture.backend->Present(frame).HasValue());
        Check(commandState.fenceCount == 2);
        Check(commandState.deleteFenceCount == 1);
        Check(fixture.backend->Present(frame).HasError());
        fixture.backend->Shutdown();
        Check(commandState.deleteFenceCount == 2);
    }

    TEST_CASE("OpenGL Requires Loaded State And Sync Dispatch Before Publishing Readiness", "[unit][runtime][renderer][opengl_commands]") {
        commandState = {};
        PortState portState;
        FakePresentationPort port{portState};
        auto backend = CreateBackend(port);
        commandState.dispatchAvailable = false;
        const auto initialized = backend->Initialize({});
        Check(initialized.HasError());
        Check(initialized.ErrorValue().code.Value() == "render.opengl.missing_required_entry_points");
        Check(portState.destroyCount == portState.createCount);
        Check(!backend->Capabilities().support.IsValid());
        commandState.dispatchAvailable = true;
        Check(backend->Initialize({}).HasValue());
    }

    TEST_CASE("OpenGL Foreign Threads Cannot Encode Retire Or Destroy Owner Work", "[unit][runtime][renderer][opengl_commands]") {
        CommandFixture fixture;
        const FrameToken frame = fixture.Begin();
        bool rejected = false;
        std::thread foreign{[&] {
            const auto presented = fixture.backend->Present(frame);
            rejected = presented.HasError() && presented.ErrorValue().code.Value() == "render.opengl.wrong_thread";
            fixture.backend->AbortFrame(frame);
            fixture.backend->Shutdown();
        }};
        foreign.join();
        Check(rejected);
        Check(commandState.fenceCount == 0);
        Check(fixture.portState.destroyCount == 0);
        Check(fixture.backend->Present(frame).HasValue());
        Check(commandState.fenceCount == 1);
    }
}  // namespace Horo::Render::OpenGLBackendTests
