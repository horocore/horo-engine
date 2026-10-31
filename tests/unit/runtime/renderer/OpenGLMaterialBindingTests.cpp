#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "AllocationProbe.h"
#include "Horo/Runtime/Render/RenderShaderResourceErrors.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Vfx/VfxWorld.h"
#include "OpenGLBackendInternal.h"
#include "OpenGLMaterialFixture.h"
#include "OpenGLRenderBackendErrors.h"
#include "OpenGLRenderTestSupport.h"
#include "OpenGLResourceGraphTestSupport.h"
#include "RenderFrontendErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderTransientGraphTestSupport.h"
#include "TypedIdentityTestSupport.h"

#include <functional>
#include <limits>
#include <optional>
#include <thread>

namespace Horo::Render::OpenGLResourceTests {
    /** @brief Borrows the existing deterministic resource dispatch; never creates a real context. */
    Detail::OpenGLCommandFunctions ResourceProbeFunctions() noexcept;
}  // namespace Horo::Render::OpenGLResourceTests

namespace Horo::Render::OpenGLMaterialTests {
// The following cases and VFX chunks share this deterministic dispatch and resource fixture.
#include "OpenGLMaterialDispatchFixture.inl"

    TEST_CASE("Prepared OpenGL material consumption retains sampler and program through exact completion", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        REQUIRE(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}).HasValue());
        REQUIRE(native.draws == 1);
        Horo::Tests::RequireError(frontend->ReleaseSampler(fixture.sampler), FrontendErrors::ResourceChangeDuringFrame);
        Horo::Tests::RequireError(frontend->ReleaseGraphicsPipeline(fixture.pipeline.pipeline), FrontendErrors::ResourceChangeDuringFrame);
        Horo::Tests::RequireError(frontend->ReleaseShaderModule(fixture.pipeline.vertex), FrontendErrors::ResourceChangeDuringFrame);
        frame.Cancel();
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        CHECK(native.shadersDeleted == 0);
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.samplersDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        // Completion authority polls fences at frame admission; upload/registry drains do not advance native completion.
        auto completed = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(completed.HasValue());
        auto nextFrame = std::move(completed).Value();
        nextFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

    TEST_CASE("Partial OpenGL material failure and exception retain generations through fence failure and shutdown",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        SECTION("Native failure") {
            native.failDraw = true;
        }
        SECTION("Owned native metadata exception") {
            native.throwDraw = true;
        }
        OpenGLBackendTests::commandState.fenceFails = true;
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        CHECK(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}).HasError());
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        const auto failedClose = frontend->BeginShutdown();
        Horo::Tests::RequireError(failedClose, OpenGLBackendErrors::SynchronizationFailed);
        const auto pending = frontend->PollDrain();
        REQUIRE(pending.HasValue());
        CHECK(pending.Value() == RenderBackendDrainProgress::Pending);
        CHECK(native.samplersDeleted == 0);
        CHECK(native.programsDeleted == 0);
        OpenGLBackendTests::commandState.fenceFails = false;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        REQUIRE(frontend->BeginShutdown().HasValue());
        const auto closing = frontend->PollDrain();
        REQUIRE(closing.HasValue());
        CHECK(closing.Value() == RenderBackendDrainProgress::Pending);
        CHECK(native.programsDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        Horo::Render::Test::RequireDrainedOwner(*frontend);
        frontend.reset();
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

    TEST_CASE("OpenGL material exception boundary cleans up before and after lease transfer", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        SECTION("Before transfer") {
            port.throwOnMakeCurrent = true;
        }
        SECTION("Standard native exception after transfer") {
            native.exception = DrawException::Standard;
        }
        SECTION("Nonstandard native exception after transfer") {
            native.exception = DrawException::Nonstandard;
        }
        SECTION("Allocation exception after transfer") {
            native.exception = DrawException::Allocation;
        }
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        const auto result = frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws});
        Horo::Tests::RequireError(result, FrontendErrors::FrameException);
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        frame.Cancel();
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        const bool transferred = !port.throwOnMakeCurrent;
        port.throwOnMakeCurrent = false;
        native.exception = DrawException::None;
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.programsDeleted == (transferred ? 0 : 1));
        CHECK(native.samplersDeleted == (transferred ? 0 : 1));
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        auto recovered = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(recovered.HasValue());
        auto recoveredFrame = std::move(recovered).Value();
        recoveredFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.programsDeleted == 1);
        CHECK(native.samplersDeleted == 1);
    }

    TEST_CASE("OpenGL material double allocation failure completes cleanup before public error construction escapes",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        native.failDraw = true;
        OpenGLBackendTests::commandState.pollStatus = 0x911BU;
        TranslationFailureProbe probe;
        bool allocationEscaped{};
        {
            // The existing observer ABI borrows opaque context; this capture-free adapter restores its exact stack-owned type.
            const Horo::Tests::AllocationProbe::AllocationObserver adapter = [](std::size_t bytes, auto *opaque) noexcept {
                static_cast<TranslationFailureProbe *>(opaque)->Observe(bytes);
            };
            Horo::Tests::AllocationProbe::ScopedObserver observer{adapter, &probe};
            try {
                static_cast<void>(frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws}));
            } catch (const std::bad_alloc &) {
                allocationEscaped = true;
            }
        }
        probe.failure.reset();
        CHECK(allocationEscaped);
        CHECK(probe.faults == 2);
        CHECK(probe.fencesAtTranslation == 1);
        frame.Cancel();
        CHECK(OpenGLBackendTests::commandState.fenceCount == 1);
        ReleaseCpu(*frontend, fixture);
        draws[0].material.reset();
        CHECK(native.programsDeleted == 0);
        CHECK(native.samplersDeleted == 0);
        OpenGLBackendTests::commandState.pollStatus = 0x911AU;
        auto recovered = frontend->BeginFrame({.frameNumber = 2, .outputExtent = {64, 32}});
        REQUIRE(recovered.HasValue());
        auto recoveredFrame = std::move(recovered).Value();
        recoveredFrame.Cancel();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(native.programsDeleted == 1);
        CHECK(native.samplersDeleted == 1);
    }

    TEST_CASE("OpenGL material admission rejects stale generations and mismatched layout before parameter allocation",
              "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto invalid = Material(fixture.pipeline, fixture.sample.view, fixture.sampler);
        const auto buffers = OpenGLResourceTests::resourceCommandState.generatedBuffers;
        SECTION("Exact native layout mismatch") {
            invalid.layout.targetBindings[0].nativeName = "OtherBlock";
        }
        SECTION("Stale sampler discovery") {
            REQUIRE(frontend->ReleaseSampler(fixture.sampler).HasValue());
        }
        SECTION("Stale pipeline generation") {
            ++invalid.pipeline.generation;
        }
        CHECK(frontend->Realize(invalid).HasError());
        CHECK(OpenGLResourceTests::resourceCommandState.generatedBuffers == buffers);
        CHECK(native.draws == 0);
        fixture.material.reset();
        REQUIRE(fixture.table->Shutdown().HasValue());
        fixture.table.reset();
    }

    TEST_CASE("OpenGL graphics pipeline rejects stale device and shader identities without native linking",
              "[renderer][opengl][pipeline]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto pipeline = PreparePipeline(*frontend);
        auto invalid = pipeline.descriptor;
        SECTION("Changed device identity") {
            invalid.compatibility.driverVersion = "other-driver";
        }
        SECTION("Stale shader generation") {
            ++invalid.vertex.generation;
        }
        const auto programs = native.nextProgram;
        CHECK(frontend->CreateGraphicsPipeline(invalid).HasError());
        CHECK(native.nextProgram == programs);
    }

    TEST_CASE("Internal material storage query delegates exact native layout and preserves admission errors",
              "[renderer][opengl][material][frontend]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        const auto pipeline = PreparePipeline(*frontend);
        MaterialBindingDescriptor descriptor{.pipeline = pipeline.pipeline, .layout = pipeline.layout};
        const auto query = [&] {
            return Detail::RenderFrontendResourceAccess::MaterialParameterBufferBytes(*frontend, descriptor, {1});
        };
        const auto size = query();
        REQUIRE(size.HasValue());
        REQUIRE(size.Value() == 16);
        SECTION("stale frontend generation") {
            ++descriptor.pipeline.generation;
            CHECK(query().HasError());
        }
        SECTION("incompatible native layout") {
            descriptor.layout.shaderInterface = {};
            CHECK(query().HasError());
        }
        SECTION("wrong preparation thread") {
            std::optional<Result<std::size_t>> result;
            std::thread worker{[&] {
                result.emplace(query());
            }};
            worker.join();
            REQUIRE(result.has_value());
            Horo::Tests::RequireError(*result, RenderShaderResourceErrors::WrongThread);
        }
        SECTION("active frame") {
            auto frame = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
            REQUIRE(frame.HasValue());
            Horo::Tests::RequireError(query(), FrontendErrors::ResourceChangeDuringFrame);
        }
        SECTION("closing frontend") {
            REQUIRE(frontend->BeginShutdown().HasValue());
            Horo::Tests::RequireError(query(), FrontendErrors::Closing);
        }
        SECTION("retired native pipeline") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, pipeline.pipeline);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroyGraphicsPipeline(instance.Value());
            CHECK(query().HasError());
        }
    }

    TEST_CASE("OpenGL material consumption rejects missing native instances before encoding", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        // Deliberate backend-loss injection in a fake context, not a supported CPU release path. Registry generations
        // remain live so this exercises the native-instance admission checks independently of frontend stale-handle checks.
        SECTION("Missing native sampler") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.sampler);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroySampler(instance.Value());
        }
        SECTION("Missing native program") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.pipeline.pipeline);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroyGraphicsPipeline(instance.Value());
        }
        SECTION("Missing native mesh") {
            const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.mesh);
            REQUIRE(instance.HasValue());
            borrowedBackend->DestroyMesh(instance.Value());
        }
        SECTION("Missing native parameter buffer") {
            borrowedBackend->DestroyBuffer(OpenGLResourceTests::resourceCommandState.nextObject - 1);
        }
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        std::array draws{RenderMaterialDraw{fixture.mesh, fixture.material}};
        const auto result = frame.ExecuteMaterialPass({fixture.target.target, {64, 32}, {}, 1.0F, draws});
        Horo::Tests::RequireError(result, OpenGLBackendErrors::ResourceIdentityInvalid);
        CHECK(native.draws == 0);
        draws[0].material.reset();
        fixture.material.reset();
        REQUIRE(fixture.table->Shutdown().HasValue());
        fixture.table.reset();
    }

    TEST_CASE("Cancelling before OpenGL material encoding transfers no completion ownership", "[renderer][opengl][material]") {
        OpenGLResourceTests::ResourcePresentationPort port;
        auto frontend = Create(port);
        auto fixture = Prepare(*frontend);
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        frame.Cancel();
        ReleaseCpu(*frontend, fixture);
        CHECK(native.draws == 0);
        CHECK(native.samplersDeleted == 1);
        CHECK(native.programsDeleted == 1);
        CHECK(native.shadersDeleted == 2);
    }

#include "OpenGLVfxFrameTests.inl"

// Draw preparation consumes the material/slot fixture declarations from the preceding chunk.
#include "OpenGLVfxDrawPreparationTests.inl"
}  // namespace Horo::Render::OpenGLMaterialTests
