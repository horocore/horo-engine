#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendInternal.h"
#include "OpenGLRenderTestSupport.h"
#include "renderer/RenderBackendContractSuite.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Horo::Render::OpenGLBackendTests {

    void CheckInitializedBackend(IRenderBackend &backend, PortState &portState) {
        Check(backend
                  .Initialize(RenderBackendConfig{
                      .requirePresentation = true,
                      .enableValidation = true,
                      .maxFramesInFlight = 2,
                      .presentMode = PresentMode::Immediate,
                  })
                  .HasValue());
        Check(portState.createCount == 1);
        Check(portState.makeCurrentCount == 1);
        Check(portState.presentModeCount == 1);
        Check(portState.presentMode == PresentMode::Immediate);
        Check(portState.descriptor.majorVersion == 4);
        Check(portState.descriptor.minorVersion == 1);
        Check(portState.descriptor.profile == OpenGLContextProfile::Core);
        Check(portState.descriptor.enableDebugContext);
        Check(backend.Capabilities().backend == RenderBackendId{"opengl"});
        Check(backend.Capabilities().presentsToWindow);
        Check(!backend.Capabilities().supportsOffscreenTargets);
        Check(!backend.Capabilities().supportsCompute);
    }

    void CheckFrameLifecycle(IRenderBackend &backend, PortState &portState) {
        const auto oversized = backend.BeginFrame(FrameDescriptor{
            .frameNumber = 1,
            .outputExtent = {std::numeric_limits<std::uint32_t>::max(), 720},
        });
        Check(oversized.HasError());
        Check(oversized.ErrorValue().code.Value() == "render.backend.invalid_frame_descriptor");
        Check(portState.makeCurrentCount == 1);
        Check(commandState.viewportCount == 0);

        auto begun = backend.BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {1280, 720}});
        Check(begun.HasValue());
        const FrameToken frame = begun.Value();
        Check(portState.makeCurrentCount == 2);
        Check(commandState.viewportCount == 1);
        Check(commandState.viewportWidth == 1280);
        Check(commandState.viewportHeight == 720);

        const std::array passes{
            BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, ClearColor{0.1F, 0.2F, 0.3F, 1.0F}),
            RenderPassDescriptor{
                .id = RenderPassId{2},
                .kind = RenderPassKind::Graphics,
                .primaryOutput = PrimaryOutputAttachment{.loadOperation = AttachmentLoadOperation::Load,
                                                         .storeOperation = AttachmentStoreOperation::Store},
            },
        };
        Check(backend.Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = passes}).HasValue());
        Check(commandState.clearColorCount == 1);
        Check(commandState.clearCount == 1);
        Check(commandState.clearMask == 0x00004000U);
        Check(commandState.color.red == 0.1F);
        Check(commandState.color.green == 0.2F);
        Check(commandState.color.blue == 0.3F);
        Check(commandState.color.alpha == 1.0F);
        Check(backend.Present(frame).HasValue());
        Check(portState.swapCount == 1);
        Check(backend.Resize(FramebufferExtent{1920, 1080}).HasValue());
    }

    TEST_CASE("Provider Is Inert And Backend Owns Context Presentation Lifecycle", "[unit][runtime][renderer]") {
        commandState = {};
        PortState portState;
        FakePresentationPort port{portState};
        RenderBackendRegistry registry;
        Check(Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{.majorVersion = 4, .minorVersion = 1},
                                                               ProbeFunctions())
                  .HasValue());
        Check(portState.createCount == 0);
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"opengl"});
        Check(created.HasValue());
        Check(portState.createCount == 0);
        std::unique_ptr<IRenderBackend> backend = std::move(created).Value();

        CheckInitializedBackend(*backend, portState);
        CheckFrameLifecycle(*backend, portState);
        backend->Shutdown();
        backend->Shutdown();
        Check(portState.destroyCount == 1);
    }

    TEST_CASE("Initialization Failures Preserve Typed Errors And Rollback Created Context", "[unit][runtime][renderer]") {
        constexpr std::array failures{PortFailure::Create, PortFailure::MakeCurrent, PortFailure::LoadDispatch, PortFailure::QueryFacts,
                                      PortFailure::PresentMode};
        for (const PortFailure failure : failures) {
            PortState portState{.failure = failure};
            FakePresentationPort port{portState};
            std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
            const Result<void> initialized = backend->Initialize(RenderBackendConfig{});
            Check(initialized.HasError());
            Check(initialized.ErrorValue().domain.Value() == "horo.render.test");
            Check(initialized.ErrorValue().severity == ErrorSeverity::Critical);
            Check(!portState.contextCreated);
            Check(portState.destroyCount == (failure == PortFailure::Create ? 0 : 1));

            portState.failure = PortFailure::None;
            Check(backend->Initialize(RenderBackendConfig{}).HasValue());
            backend->Shutdown();
            Check(portState.destroyCount == (failure == PortFailure::Create ? 1 : 2));
        }
    }

    [[nodiscard]] OpenGLContextFacts ValidContextFacts() noexcept {
        return {.apiFamily = OpenGLApiFamily::Desktop,
                .majorVersion = 4,
                .minorVersion = 1,
                .profile = OpenGLContextProfile::Core,
                .requiredEntryPointsAvailable = true,
                .maxTexture2DSize = 1,
                .maxColorAttachments = 1,
                .maxVertexAttributes = 1};
    }

    void CheckContextRejected(const OpenGLContextFacts facts, const std::string_view expectedError) {
        PortState state;
        state.facts = facts;
        FakePresentationPort port{state};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        const Result<void> initialized = backend->Initialize(RenderBackendConfig{});
        Check(initialized.HasError());
        Check(initialized.ErrorValue().code.Value() == expectedError);
        Check(state.queryFactsCount == 1 && state.destroyCount == 1 && !state.contextCreated);
    }

    TEST_CASE("Actual OpenGL Context Facts Are Admitted Before Readiness", "[unit][runtime][renderer][capabilities]") {
        OpenGLContextFacts facts = ValidContextFacts();
        facts.apiFamily = OpenGLApiFamily::Embedded;
        CheckContextRejected(facts, "render.opengl.unsupported_api_family");

        facts = ValidContextFacts();
        facts.majorVersion = 3;
        facts.minorVersion = 3;
        CheckContextRejected(facts, "render.opengl.unsupported_version");

        facts = ValidContextFacts();
        facts.profile = OpenGLContextProfile::Compatibility;
        CheckContextRejected(facts, "render.opengl.unsupported_profile");

        facts = ValidContextFacts();
        facts.requiredEntryPointsAvailable = false;
        CheckContextRejected(facts, "render.opengl.missing_required_entry_points");

        facts = ValidContextFacts();
        facts.maxTexture2DSize = 0;
        CheckContextRejected(facts, "render.opengl.invalid_capabilities");

        PortState newerState;
        newerState.facts = ValidContextFacts();
        newerState.facts.minorVersion = 6;
        FakePresentationPort newerPort{newerState};
        std::unique_ptr<IRenderBackend> newerBackend = CreateBackend(newerPort);
        Check(newerBackend->Initialize(RenderBackendConfig{}).HasValue());
        newerBackend->Shutdown();
    }

    TEST_CASE("Optional Debug Context Failure Retries Once After Complete Rollback", "[unit][runtime][renderer][lifecycle]") {
        PortState state{.failure = PortFailure::MakeCurrent, .failDebugAttemptOnly = true};
        FakePresentationPort port{state};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        const Result<void> initialized = backend->Initialize(RenderBackendConfig{.enableValidation = true});
        Check(initialized.HasValue());
        Check(state.createCount == 2);
        Check(state.debugCreateCount == 1);
        Check(state.destroyCount == 1);
        Check(!state.descriptor.enableDebugContext);
        backend->Shutdown();
        Check(state.destroyCount == 2);
    }

    TEST_CASE("Failed Debug Retry Preserves Both Typed Failures", "[unit][runtime][renderer][lifecycle]") {
        PortState state{.failure = PortFailure::Create};
        FakePresentationPort port{state};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        const Result<void> initialized = backend->Initialize(RenderBackendConfig{.enableValidation = true});
        Check(initialized.HasError());
        Check(initialized.ErrorValue().code.Value() == "render.test.create_failed");
        Check(initialized.ErrorValue().cause.Get() != nullptr);
        Check(initialized.ErrorValue().cause.Get()->code.Value() == "render.test.create_failed");
        Check(state.createCount == 2);
        Check(state.debugCreateCount == 1);
    }

    TEST_CASE("OpenGL Frame Work Rejects Non Owner Threads", "[unit][runtime][renderer][lifecycle]") {
        PortState state;
        FakePresentationPort port{state};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());

        std::string errorCode;
        std::thread worker{[&] {
            const Result<FrameToken> begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {64, 64}});
            errorCode = begun.HasError() ? begun.ErrorValue().code.Value() : std::string{};
            backend->Shutdown();
        }};
        worker.join();
        Check(errorCode == "render.opengl.wrong_thread");
        Check(state.destroyCount == 0);
        backend->Shutdown();
        Check(state.destroyCount == 1);
    }

    TEST_CASE("Initialization Exception Rolls Back Retained Context", "[unit][runtime][renderer]") {
        PortState portState{.failure = PortFailure::CreateThrowsAfterRetain};
        FakePresentationPort port{portState};
        RenderBackendRegistry registry;
        Check(Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, ProbeFunctions()).HasValue());
        Check(registry.Seal().HasValue());

        const auto frontend = RenderFrontend::Create(registry, RenderBackendId{"opengl"}, RenderBackendConfig{});
        Check(frontend.HasError());
        Check(frontend.ErrorValue().code.Value() == "render.frontend.initialize_exception");
        Check(!portState.contextCreated);
        Check(portState.destroyCount == 1);
    }

    TEST_CASE("Incomplete OpenGL Command Dispatch Rejects Generic Resources", "[unit][runtime][renderer][resource]") {
        PortState portState;
        FakePresentationPort port{portState};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        const std::array<std::byte, 4> bytes{};

        Check(!backend->Capabilities().supportsBufferResources);
        Check(!backend->Capabilities().supportsMeshResources);
        const auto buffer =
            backend->CreateBuffer({.byteSize = bytes.size(), .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::DeviceLocal},
                                  bytes,
                                  {.pool = {{1}, 1},
                                   .scope = {1, 1},
                                   .attempt = {1},
                                   .compatibility = {1},
                                   .budgetRevision = 1,
                                   .payloadBytes = bytes.size(),
                                   .requiredBytes = bytes.size(),
                                   .backingBytes = bytes.size(),
                                   .allocationClass = RenderMemoryAllocationClass::Dedicated});
        Check(buffer.HasError());
        Check(buffer.ErrorValue().code.Value() == "render.opengl.unsupported_resource_operation");
        const auto mesh = backend->CreateMesh({}, 1, 2);
        Check(mesh.HasError());
        Check(mesh.ErrorValue().code.Value() == "render.opengl.unsupported_resource_operation");
        backend->DestroyBuffer(1);
        backend->DestroyMesh(2);
    }

    TEST_CASE("Shared Presentation Port Rejects Overlapping Initialized Backends", "[unit][runtime][renderer]") {
        PortState portState;
        FakePresentationPort port{portState};
        BackendTestSupport::RunSharedPresentationLeaseContract(RenderBackendId{"opengl"}, "render.opengl.presentation_in_use",
                                                               [&port](RenderBackendRegistry &registry) {
            return Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, ProbeFunctions());
        }, [&portState] {
            Check(portState.destroyCount == 2);
        });
    }

    TEST_CASE("Plan Validation Precedes Commands And Presentation Failure Keeps Recovery Token", "[unit][runtime][renderer]") {
        commandState = {};
        PortState portState;
        FakePresentationPort port{portState};
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port);
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());

        portState.failure = PortFailure::MakeCurrent;
        const Result<FrameToken> currentFailure = backend->BeginFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {800, 600}});
        Check(currentFailure.HasError());
        Check(currentFailure.ErrorValue().code.Value() == "render.test.current_failed");
        Check(commandState.viewportCount == 0);
        portState.failure = PortFailure::None;

        auto begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        const FrameToken frame = begun.Value();

        const std::array invalidPasses{
            RenderPassDescriptor{
                .id = RenderPassId{3},
                .kind = RenderPassKind::Graphics,
                .primaryOutput =
                    PrimaryOutputAttachment{
                        .loadOperation = AttachmentLoadOperation::Clear,
                        .clearColor = ClearColor{0.4F, 0.5F, 0.6F, 1.0F},
                    },
            },
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Compute},
        };
        const Result<void> invalid = backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = invalidPasses});
        Check(invalid.HasError());
        Check(invalid.ErrorValue().code.Value() == "render.opengl.unsupported_pass_kind");
        Check(commandState.clearColorCount == 0);
        Check(commandState.clearCount == 0);

        const std::array validPass{
            RenderPassDescriptor{
                .id = RenderPassId{5},
                .kind = RenderPassKind::Graphics,
                .primaryOutput = PrimaryOutputAttachment{},
            },
        };
        Check(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = validPass}).HasValue());
        portState.failure = PortFailure::Swap;
        const Result<void> failedPresent = backend->Present(frame);
        Check(failedPresent.HasError());
        Check(failedPresent.ErrorValue().code.Value() == "render.test.swap_failed");
        Check(failedPresent.ErrorValue().domain.Value() == "horo.render.test");

        portState.failure = PortFailure::None;
        Check(backend->Present(frame).HasValue());
        Check(portState.swapCount == 2);
    }

    TEST_CASE("Registration Rejects Invalid Command Dispatch Without Owning The Port", "[unit][runtime][renderer]") {
        PortState portState;
        FakePresentationPort port{portState};
        RenderBackendRegistry registry;
        const Result<void> rejected =
            Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, Detail::OpenGLCommandFunctions{});
        Check(rejected.HasError());
        Check(rejected.ErrorValue().code.Value() == "render.opengl.invalid_registration");
        Check(portState.createCount == 0);

        const Result<void> legacy =
            Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{.majorVersion = 3, .minorVersion = 3},
                                                             ProbeFunctions());
        Check(legacy.HasError());
        Check(legacy.ErrorValue().code.Value() == "render.opengl.invalid_registration");

        Check(Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, ProbeFunctions()).HasValue());
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"opengl"});
        Check(created.HasValue());
        const Result<void> invalidConfig =
            std::move(created).Value()->Initialize(RenderBackendConfig{.presentMode = static_cast<PresentMode>(0xFF)});
        Check(invalidConfig.HasError());
        Check(invalidConfig.ErrorValue().code.Value() == "render.backend.invalid_config");
        Check(portState.createCount == 0);
    }

    TEST_CASE("Module Info Describes Open GL Window Before Backend Creation", "[unit][runtime][renderer]") {
        const RenderBackendModuleInfo &info = GetOpenGLRenderBackendModuleInfo();
        Check(info.id == RenderBackendId{"opengl"});
        Check(info.windowRequirements.presentation == RenderPresentationKind::OpenGL);
        Check(info.windowRequirements.resizable && info.windowRequirements.highPixelDensity);
        Check(info.supportsInteractivePresentation);
    }

}  // namespace Horo::Render::OpenGLBackendTests
