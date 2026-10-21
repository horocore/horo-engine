#include "../../../support/renderer/RenderBackendDrainTestSupport.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "MetalRenderTestSupport.h"
#include "RenderMemoryTestSupport.h"
#include "renderer/RenderBackendContractSuite.h"
#include "runtime/renderer/modules/metal/MetalBackendInternal.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <string_view>

#if !defined(__APPLE__)
namespace Horo::Render::Detail {
    /** @brief Linux-only contract-test stub for the unavailable native Metal runtime. */
    Result<std::unique_ptr<IMetalRuntime>> CreateMetalRuntime(IMetalPresentationPort &, MetalEditorGraphicsBridge &) {
        return Result<std::unique_ptr<IMetalRuntime>>::Failure(
            MakeError(MetalBackendErrors::UnsupportedHost, "Native Metal runtime is unavailable in Linux contract tests."));
    }
}  // namespace Horo::Render::Detail
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::MetalBackendTests;

    /** @brief Captures preparation transport independently of any native Metal implementation. */
    struct PreparationProbe {
        std::array<unsigned, 8> calls{};
        std::array<std::uint64_t, 2> stages{};
        std::size_t cookedBytes{0};
        std::uint64_t destroyed{0};
    };

    /** @brief Returns an exact injected failure so inherited unsupported defaults cannot satisfy delegation tests. */
    class PreparationRuntime final : public FakeMetalRuntime {
    public:
        PreparationRuntime(IMetalPresentationPort &port, PortState &state, PreparationProbe &probe)
            : FakeMetalRuntime(port, state), probe_(&probe) {}

        Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &, std::size_t) const override {
            return Observe<RenderMemoryCostPlan>(0);
        }

        Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &, std::uint64_t,
                                                                     std::uint64_t) const override {
            return Observe<RenderMemoryCostPlan>(1);
        }

        Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &) const override {
            return Observe<RenderMemoryCostPlan>(2);
        }

        Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &, std::span<const std::uint8_t> code,
                                                 const RenderMemoryPlacement &) override {
            probe_->cookedBytes = code.size();
            return Observe<std::uint64_t>(3);
        }

        Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &, std::uint64_t vertex, std::uint64_t fragment,
                                                     const RenderMemoryPlacement &) override {
            probe_->stages = {vertex, fragment};
            return Observe<std::uint64_t>(4);
        }

        Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &, const RenderMemoryPlacement &) override {
            return Observe<std::uint64_t>(5);
        }

        Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t, const MaterialBindingDescriptor &,
                                                             ShaderBindingId) const override {
            return Observe<std::size_t>(6);
        }

        Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &, std::uint64_t,
                                                                          std::span<const MaterialResidentResource>) override {
            return Observe<std::unique_ptr<IResidentMaterialBinding>>(7);
        }

        void DestroyShaderModule(std::uint64_t identity) noexcept override {
            probe_->destroyed += identity;
        }

        void DestroyGraphicsPipeline(std::uint64_t identity) noexcept override {
            probe_->destroyed += identity;
        }

        void DestroySampler(std::uint64_t identity) noexcept override {
            probe_->destroyed += identity;
        }

    private:
        /** @brief Records a synchronous owner call and preserves a distinct native error identity. */
        template <typename T> Result<T> Observe(std::size_t operation) const {
            ++probe_->calls[operation];
            return Result<T>::Failure(MakePortError("render.test.preparation_failed", "Exact selected-runtime preparation failure."));
        }

        PreparationProbe *probe_;
    };

    /** @brief Installs only the explicitly injected transport probe. */
    class PreparationFactory final : public Detail::IMetalRuntimeFactory {
    public:
        PreparationFactory(PortState &state, PreparationProbe &probe) : state_(&state), probe_(&probe) {}

        Result<std::unique_ptr<Detail::IMetalRuntime>> Create(IMetalPresentationPort &port, MetalEditorGraphicsBridge &) const override {
            return Result<std::unique_ptr<Detail::IMetalRuntime>>::Success(std::make_unique<PreparationRuntime>(port, *state_, *probe_));
        }

    private:
        PortState *state_;
        PreparationProbe *probe_;
    };

    /** @brief Checks the native error survives resource and material preparation result types. */
    template <typename T> void CheckPreparationError(const Result<T> &result, const std::string_view code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }

    /** @brief Exercises each preparation method without introducing native policy into the transport fixture. */
    void CheckPreparationSurface(IRenderBackend &backend, std::string_view code) {
        constexpr std::array<std::uint8_t, 4> bytes{};
        CheckPreparationError(backend.QueryShaderModuleMemoryCost({}, bytes.size()), code);
        CheckPreparationError(backend.QueryGraphicsPipelineMemoryCost({}, 11, 13), code);
        CheckPreparationError(backend.QuerySamplerMemoryCost({}), code);
        CheckPreparationError(backend.CreateShaderModule({}, bytes, {}), code);
        CheckPreparationError(backend.CreateGraphicsPipeline({}, 17, 19, {}), code);
        CheckPreparationError(backend.CreateSampler({}, {}), code);
        CheckPreparationError(backend.QueryMaterialParameterBufferSize(23, {}, ShaderBindingId{1}), code);
        CheckPreparationError(backend.RealizeResolved({}, 29, {}), code);
    }

    using GenericResourceIdentities = BackendTestSupport::RenderResourceIdentities;

    void CreateGenericBuffersAndMesh(IRenderBackend &backend, GenericResourceIdentities &identities) {
        constexpr std::array<std::byte, 12> bytes{};
        const RenderBufferDescriptor vertexDescriptor = BackendTestSupport::MakeTestVertexBufferDescriptor(bytes.size());
        const RenderBufferDescriptor indexDescriptor = BackendTestSupport::MakeTestIndexBufferDescriptor(bytes.size());
        const auto vertexPlan = backend.QueryBufferMemoryCost(vertexDescriptor).Value();
        const auto indexPlan = backend.QueryBufferMemoryCost(indexDescriptor).Value();
        const auto vertex = backend.CreateBuffer(vertexDescriptor, bytes, TestSupport::PlacementFor(vertexPlan, 1));
        const auto index = backend.CreateBuffer(indexDescriptor, bytes, TestSupport::PlacementFor(indexPlan, 2));
        Check(vertex.HasValue() && index.HasValue());
        identities.vertex = vertex.Value();
        identities.index = index.Value();
        const auto mesh = backend.CreateMesh({.vertexBuffer = {{1}, 1, 1},
                                              .indexBuffer = {{1}, 2, 1},
                                              .vertexStride = sizeof(float) * 3,
                                              .vertexCount = 1,
                                              .indexCount = 3,
                                              .localBounds = {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}}},
                                             identities.vertex, identities.index);
        Check(mesh.HasValue());
        identities.mesh = mesh.Value();
    }

    void CreateGenericTexturesAndTarget(IRenderBackend &backend, GenericResourceIdentities &identities) {
        const RenderTextureDescriptor colorDescriptor{.extent = {64, 64},
                                                      .format = RenderTextureFormat::Rgba8Unorm,
                                                      .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
        const RenderTextureDescriptor depthDescriptor{.extent = {64, 64},
                                                      .format = RenderTextureFormat::Depth32Float,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        const auto colorPlan = backend.QueryTextureMemoryCost(colorDescriptor).Value();
        const auto depthPlan = backend.QueryTextureMemoryCost(depthDescriptor).Value();
        const auto color = backend.CreateTexture(colorDescriptor, {}, TestSupport::PlacementFor(colorPlan, 3));
        const auto depth = backend.CreateTexture(depthDescriptor, {}, TestSupport::PlacementFor(depthPlan, 4));
        Check(color.HasValue() && depth.HasValue());
        identities.color = color.Value();
        identities.depth = depth.Value();
        const auto colorView = backend.CreateTextureView({.texture = {{1}, 1, 1},
                                                          .format = RenderTextureFormat::Rgba8Unorm,
                                                          .aspect = RenderTextureAspect::Color},
                                                         identities.color);
        const auto depthView = backend.CreateTextureView({.texture = {{1}, 2, 1},
                                                          .format = RenderTextureFormat::Depth32Float,
                                                          .aspect = RenderTextureAspect::Depth},
                                                         identities.depth);
        Check(colorView.HasValue() && depthView.HasValue());
        identities.colorView = colorView.Value();
        identities.depthView = depthView.Value();
        const auto target = backend.CreateRenderTarget({.colorAttachment = {{1}, 1, 1}, .depthAttachment = {{1}, 2, 1}, .extent = {64, 64}},
                                                       identities.colorView, identities.depthView);
        Check(target.HasValue());
        identities.target = target.Value();
    }

    [[nodiscard]] GenericResourceIdentities CreateGenericResources(IRenderBackend &backend) {
        GenericResourceIdentities identities;
        CreateGenericBuffersAndMesh(backend, identities);
        CreateGenericTexturesAndTarget(backend, identities);
        return identities;
    }

    void DestroyGenericResources(IRenderBackend &backend, const GenericResourceIdentities &resources) {
        BackendTestSupport::DestroyRenderResources(backend, resources);
    }

    void ExerciseFrameLifecycle(IRenderBackend &backend, const PortState &state) {
        Check(backend.Resize(FramebufferExtent{1920, 1080}).HasValue());
        Check(state.resizeCount == 1);
        Check(state.resizedExtent.width == 1920 && state.resizedExtent.height == 1080);

        auto begun = backend.BeginFrame(FrameDescriptor{.frameNumber = 7, .outputExtent = {1280, 720}});
        Check(begun.HasValue());
        const FrameToken frame = begun.Value();
        Check(state.beginCount == 1);
        Check(state.frameExtent.width == 1280 && state.frameExtent.height == 720);

        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, ClearColor{0.1F, 0.2F, 0.3F, 1.0F})};
        Check(backend.Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = passes}).HasValue());
        Check(state.executeCount == 1);
        Check(state.attachment.clearColor.red == 0.1F);
        Check(state.attachment.clearColor.green == 0.2F);
        Check(state.attachment.clearColor.blue == 0.3F);
        Check(backend.Present(frame).HasValue());
        Check(state.presentCount == 1);
    }

    TEST_CASE("Provider Is Inert And Backend Owns Presentation Lifecycle", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        FakeMetalRuntimeFactory runtimeFactory{state};
        RenderBackendRegistry registry;
        Check(Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, port, bridge, runtimeFactory).HasValue());
        Check(state.createCount == 0);
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"metal"});
        Check(created.HasValue());
        Check(state.createCount == 0);
        std::unique_ptr<IRenderBackend> backend = std::move(created).Value();

        Check(backend
                  ->Initialize(RenderBackendConfig{
                      .requirePresentation = true,
                      .enableValidation = true,
                      .maxFramesInFlight = 3,
                      .presentMode = PresentMode::Immediate,
                  })
                  .HasValue());
        Check(state.createCount == 1);
        Check(state.descriptor.enableValidation);
        Check(state.descriptor.maxFramesInFlight == 3);
        Check(state.descriptor.presentMode == PresentMode::Immediate);
        Check(backend->Capabilities().backend == RenderBackendId{"metal"});
        Check(backend->Capabilities().presentsToWindow);

        ExerciseFrameLifecycle(*backend, state);

        Horo::Render::Test::RequireDrainedOwner(*backend);
        Horo::Render::Test::RequireDrainedOwner(*backend);
        Check(state.destroyCount == 1);
        Check(!backend->Capabilities().presentsToWindow);
    }

    TEST_CASE("Explicit Metal Adapter Selection Is Revalidated Without Fallback", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port, state, bridge);
        Check(backend
                  ->Initialize(RenderBackendConfig{
                      .adapter = RenderAdapterId{"metal:test"},
                      .adapterDiscoveryRevision = 1,
                      .requirePresentation = true,
                  })
                  .HasValue());
        Horo::Render::Test::RequireDrainedOwner(*backend);

        const Result<void> stale = backend->Initialize(RenderBackendConfig{
            .adapter = RenderAdapterId{"metal:test"},
            .adapterDiscoveryRevision = 2,
            .requirePresentation = true,
        });
        Check(stale.HasError());
        Check(stale.ErrorValue().code.Value() == "render.test.adapter_mismatch");
        Check(state.destroyCount == 2);
    }

    TEST_CASE("Failures Preserve Typed Errors And Frame Recovery", "[unit][runtime][renderer]") {
        PortState state{.failure = PortFailure::Create};
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port, state, bridge);
        const Result<void> createFailure = backend->Initialize(RenderBackendConfig{});
        Check(createFailure.HasError());
        Check(createFailure.ErrorValue().code.Value() == "render.test.create_failed");
        Check(state.destroyCount == 0);

        state.failure = PortFailure::None;
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());
        state.failure = PortFailure::Begin;
        const Result<FrameToken> beginFailure = backend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {800, 600}});
        Check(beginFailure.HasError());
        Check(beginFailure.ErrorValue().code.Value() == "render.test.begin_failed");

        state.failure = PortFailure::None;
        auto begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        const FrameToken frame = begun.Value();
        const std::array pass{RenderPassDescriptor{
            .id = RenderPassId{2},
            .kind = RenderPassKind::Graphics,
            .primaryOutput = PrimaryOutputAttachment{},
        }};
        Check(backend->Execute(RenderExecutionPlan{.frame = frame, .orderedPasses = pass}).HasValue());

        state.failure = PortFailure::Present;
        const Result<void> presentFailure = backend->Present(frame);
        Check(presentFailure.HasError());
        Check(presentFailure.ErrorValue().code.Value() == "render.test.present_failed");
        backend->AbortFrame(frame);
        Check(state.abortCount == 1);

        state.failure = PortFailure::None;
        Check(backend->BeginFrame(FrameDescriptor{.frameNumber = 3, .outputExtent = {800, 600}}).HasValue());
        backend->AbortActiveFrame();
        Check(state.abortCount == 2);
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Shared Presentation Port Rejects Overlapping Backends", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        FakeMetalRuntimeFactory runtimeFactory{state};
        BackendTestSupport::RunSharedPresentationLeaseContract(RenderBackendId{"metal"}, "render.metal.presentation_in_use",
                                                               [&port, &bridge, &runtimeFactory](RenderBackendRegistry &registry) {
            return Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, port, bridge, runtimeFactory);
        }, [&state] {
            Check(state.destroyCount == 2);
        });
    }

    TEST_CASE("Invalid Plans Do Not Reach The Presentation Port", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<IRenderBackend> backend = CreateInitializedBackend(port, state, bridge);
        auto begun = backend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {640, 480}});
        Check(begun.HasValue());

        const std::array invalidPasses{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Graphics, .primaryOutput = PrimaryOutputAttachment{}},
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Compute},
        };
        const Result<void> invalid = backend->Execute(RenderExecutionPlan{.frame = begun.Value(), .orderedPasses = invalidPasses});
        Check(invalid.HasError());
        Check(invalid.ErrorValue().code.Value() == "render.metal.unsupported_pass_kind");
        Check(state.executeCount == 0);
        backend->AbortActiveFrame();
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Generic Resources Delegate To Metal Runtime", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<IRenderBackend> backend = CreateInitializedBackend(port, state, bridge);
        Check(backend->Capabilities().supportsBufferResources);
        Check(backend->Capabilities().supportsMeshResources);
        Check(backend->Capabilities().supportsTextureResources);
        Check(backend->Capabilities().supportsRenderTargetResources);

        const GenericResourceIdentities resources = CreateGenericResources(*backend);
        Check(state.resourceCreateCount == 8);
        DestroyGenericResources(*backend, resources);
        Check(state.resourceDestroyCount == 8);
        Horo::Render::Test::RequireDrainedOwner(*backend);
    }

    TEST_CASE("Module Info Describes Metal Window Before Backend Creation", "[unit][runtime][renderer]") {
        const RenderBackendModuleInfo &info = GetMetalRenderBackendModuleInfo();
        Check(info.id == RenderBackendId{"metal"});
        Check(info.windowRequirements.presentation == RenderPresentationKind::Metal);
        Check(info.windowRequirements.resizable && info.windowRequirements.highPixelDensity);
        Check(info.supportsInteractivePresentation);
    }

    TEST_CASE("Unsupported Drawable Buffer Count Fails Before Presentation Creation", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port, state, bridge);
        const Result<void> initialized = backend->Initialize(RenderBackendConfig{.maxFramesInFlight = 4});
        Check(initialized.HasError());
        Check(initialized.ErrorValue().code.Value() == "render.metal.unsupported_frames_in_flight");
        Check(state.createCount == 0);
    }

    TEST_CASE("Metal preparation preserves exact runtime failures and lifecycle admission", "[unit][runtime][renderer]") {
        PortState state;
        PreparationProbe probe;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        PreparationFactory factory{state, probe};
        RenderBackendRegistry registry;
        REQUIRE(Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, port, bridge, factory).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"metal"});
        REQUIRE(created.HasValue());
        auto backend = std::move(created).Value();
        CheckPreparationSurface(*backend, "render.backend.not_initialized");
        REQUIRE(backend->Initialize({}).HasValue());
        CheckPreparationSurface(*backend, "render.test.preparation_failed");
        CHECK(probe.cookedBytes == 4);
        CHECK((probe.stages == std::array<std::uint64_t, 2>{17, 19}));
        backend->DestroyShaderModule(101);
        backend->DestroyGraphicsPipeline(103);
        backend->DestroySampler(107);
        CHECK(probe.destroyed == 311);
        REQUIRE(backend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}).HasValue());
        CheckPreparationSurface(*backend, "render.backend.frame_active");
        backend->AbortActiveFrame();
        Horo::Render::Test::RequireDrainedOwner(*backend);
        CheckPreparationSurface(*backend, "render.backend.not_initialized");
        backend->DestroyShaderModule(109);
        backend->DestroyGraphicsPipeline(113);
        backend->DestroySampler(127);
        CHECK(probe.destroyed == 311);
        for (const auto count : probe.calls)
            CHECK(count == 1);
    }

    TEST_CASE("Metal preparation rejects active frames and shutdown before touching an unsupported runtime", "[unit][runtime][renderer]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        auto backend = CreateBackend(port, state, bridge);
        const auto cold = backend->QueryPipelineDeviceIdentity();
        REQUIRE(cold.HasError());
        CHECK(cold.ErrorValue().code.Value() == "render.backend.not_initialized");
        REQUIRE(backend->Initialize(RenderBackendConfig{}).HasValue());
        const auto preparedIdentity = backend->QueryPipelineDeviceIdentity();
        REQUIRE(preparedIdentity.HasValue());
        CHECK(preparedIdentity.Value().driverIdentity == "injected.metal.preparation");
        // A selected runtime without sampler preparation support preserves its typed unsupported result.
        const auto unsupported = backend->QuerySamplerMemoryCost({});
        REQUIRE(unsupported.HasError());
        const auto frame = backend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {640, 480}});
        REQUIRE(frame.HasValue());
        const auto active = backend->QuerySamplerMemoryCost({});
        REQUIRE(active.HasError());
        CHECK(active.ErrorValue().code.Value() == "render.backend.frame_active");
        backend->AbortActiveFrame();
        Horo::Render::Test::RequireDrainedOwner(*backend);
        const auto closed = backend->RealizeResolved({}, 0, {});
        REQUIRE(closed.HasError());
        CHECK(closed.ErrorValue().code.Value() == "render.backend.not_initialized");
    }

}  // namespace

TEST_CASE("Metal validates exact primary declarations and finite plan bounds before any encoding", "[metal][presentation]") {
    PortState state;
    FakePresentationPort port{state};
    MetalEditorGraphicsBridge bridge;
    auto backend = CreateInitializedBackend(port, state, bridge);
    const auto frame = backend->BeginFrame({1, {64, 64}});
    REQUIRE(frame.HasValue());
    SECTION("unadmitted explicit encoding rejects the entire plan") {
        const std::array passes{BackendTestSupport::MakeClearGraphicsPass(RenderPassId{1}, {}),
                                RenderPassDescriptor{.id = RenderPassId{2},
                                                     .primaryOutput = PrimaryOutputAttachment{.outputColor = RenderOutputColor{}}}};
        const auto rejected = backend->Execute({frame.Value(), passes});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnsupported.code.Value());
    }
    SECTION("oversized metadata rejects before quadratic identity validation") {
        std::array<RenderPassDescriptor, 1025> passes;
        const auto rejected = backend->Execute({frame.Value(), passes});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == MetalBackendErrors::InvalidExecutionPlan.code.Value());
    }
    CHECK(state.executeCount == 0);
    backend->AbortFrame(frame.Value());
    REQUIRE(backend->BeginShutdown().HasValue());
    REQUIRE(backend->PollDrain().Value() == RenderBackendDrainProgress::Drained);
}
