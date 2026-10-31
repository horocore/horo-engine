#pragma once
/** @file VulkanExternalGraphBackendTestSupport.h
 * @brief Existing composition adapter forwarding resources/submissions to the actual Vulkan device owners.
 */
#include "../renderer/VulkanExternalGraphTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

namespace Horo::Render::VulkanExternalTest {
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;

    /** @brief Test composition adapter; all image, graph, submission and retirement behavior delegates to production owners. */
    class ExternalGraphBackend final : public IRenderBackend, public Detail::IRenderExternalTextureBackend {
    public:
        static inline const Detail::IVulkanExternalTextureSource
            *lastImportedSource{};  // Synchronous test-only observation, never an owner.

        explicit ExternalGraphBackend(NativeGraphFixture &fixture, RenderBackendId backend = RenderBackendId{"xr-bridge-test"},
                                      std::function<void()> releaseDrainedChildren = {})
            : fixture_(&fixture), releaseDrainedChildren_(std::move(releaseDrainedChildren)) {
            capabilities_.backend = std::move(backend);
            capabilities_.support.deviceIncarnation = 11;
            capabilities_.support.capabilityRevision = 1;
            capabilities_.support.features.Enable(RenderCapability::BufferResources);
            capabilities_.support.features.Enable(RenderCapability::MeshResources);
            capabilities_.support.features.Enable(RenderCapability::TextureResources);
            capabilities_.support.features.Enable(RenderCapability::RenderTargetResources);
            capabilities_.support.features.Enable(RenderCapability::ViewIndexedShaders);
            capabilities_.support.limits = {65536, 2048, 1, 3, 2, 2};
            capabilities_.support.formats.sampleCountMask = 2;
            capabilities_.support.formats.usages.fill(RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled);
            capabilities_.supportsBufferResources = true;
            capabilities_.supportsMeshResources = true;
            capabilities_.supportsRenderTargetResources = true;
            capabilities_.supportsTextureResources = true;
            capabilities_.supportsExplicitGraphSubmissions = true;
            capabilities_.supportsExplicitMaterialSubmissions = true;
            capabilities_.support.synthetic = true;
            if (fixture.graph.MultiviewSupport().instancedTargetRouting)
                capabilities_.support.features.Enable(RenderCapability::InstancedViewRouting);
            capabilities_.supportsSubmissionObservation = true;
        }

        Result<void> Initialize(const RenderBackendConfig &) override {
            return Result<void>::Success();
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const override {
            return fixture_->resources.QueryPipelineDeviceIdentity();
        }

        Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &d, std::size_t bytes) const override {
            return fixture_->resources.QueryShaderModuleMemoryCost(d, bytes);
        }

        Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &d, std::uint64_t v,
                                                                     std::uint64_t f) const override {
            return fixture_->resources.QueryGraphicsPipelineMemoryCost(d, v, f);
        }

        Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &d) const override {
            return fixture_->resources.QuerySamplerMemoryCost(d);
        }

        Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &d, std::span<const std::uint8_t> bytes,
                                                 const RenderMemoryPlacement &p) override {
            return fixture_->resources.CreateShaderModule(d, bytes, p);
        }

        Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &d, std::uint64_t v, std::uint64_t f,
                                                     const RenderMemoryPlacement &p) override {
            return fixture_->resources.CreateGraphicsPipeline(d, v, f, p);
        }

        Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &d, const RenderMemoryPlacement &p) override {
            return fixture_->resources.CreateSampler(d, p);
        }

        void DestroyShaderModule(std::uint64_t id) noexcept override {
            fixture_->resources.DestroyShaderModule(id);
        }

        void DestroyGraphicsPipeline(std::uint64_t id) noexcept override {
            fixture_->resources.DestroyGraphicsPipeline(id);
        }

        void DestroySampler(std::uint64_t id) noexcept override {
            fixture_->resources.DestroySampler(id);
        }

        Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t p, const MaterialBindingDescriptor &d,
                                                             ShaderBindingId binding) const override {
            return fixture_->resources.QueryMaterialParameterBufferSize(p, d, binding);
        }

        Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(const MaterialBindingDescriptor &d, std::uint64_t p,
                                                                          std::span<const MaterialResidentResource> resources) override {
            return fixture_->resources.RealizeResolved(d, p, resources);
        }

        Result<void> PrepareGuiComposition(const GuiCompositionRequest &request) override {
            return fixture_->graph.PrepareGuiComposition(request);
        }

        Result<void> ExecuteMaterialPass(const RenderMaterialExecutionRequest &request) override {
            return fixture_->graph.ExecuteMaterialPass(request);
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &d) const override {
            return fixture_->resources.QueryBufferMemoryCost(d);
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &d) const override {
            return fixture_->resources.QueryTextureMemoryCost(d);
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &d, std::span<const std::byte> b, const RenderMemoryPlacement &p,
                                           RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateBuffer(d, b, p, std::move(l));
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &d, std::uint64_t v, std::uint64_t i,
                                         RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateMesh(d, v, i, std::move(l));
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &d, std::span<const std::byte> b, const RenderMemoryPlacement &p,
                                            RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateTexture(d, b, p, std::move(l));
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &d, std::uint64_t t,
                                                RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateTextureView(d, t, std::move(l));
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &d, std::uint64_t c, std::uint64_t z,
                                                 RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateRenderTarget(d, c, z, std::move(l));
        }

        Result<std::uint64_t> ImportExternalTexture(const Detail::IRenderExternalTextureSource &s,
                                                    RenderResourceNativeLifetime l) override {
            auto imported = fixture_->resources.ImportExternalTexture(s, std::move(l));
            lastImportedSource = imported.HasValue() ? dynamic_cast<const Detail::IVulkanExternalTextureSource *>(&s) : nullptr;
            return imported;
        }

        void DestroyBuffer(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyBuffer(i);
        }

        void DestroyMesh(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyMesh(i);
        }

        void DestroyTexture(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyTexture(i);
        }

        void DestroyTextureView(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyTextureView(i);
        }

        void DestroyRenderTarget(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyRenderTarget(i);
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &d) override {
            active_ = {d.frameNumber};
            const auto begun = fixture_->graph.BeginFrame(active_);
            return begun.HasValue() ? Result<FrameToken>::Success(active_) : Result<FrameToken>::Failure(begun.ErrorValue());
        }

        Result<void> Execute(const RenderExecutionPlan &) override {
            return Result<void>::Failure(MakeError(VulkanBackendErrors::UnsupportedOperation));
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &r) override {
            return fixture_->graph.ExecuteGraph(r);
        }

        Result<void> Present(FrameToken f) override {
            active_ = {};
            return fixture_->graph.SubmitFrame(f);
        }

        void AbortFrame(FrameToken f) noexcept override {
            fixture_->graph.AbortFrame(f);
            active_ = {};
        }

        void AbortActiveFrame() noexcept override {
            fixture_->graph.AbortFrame(active_);
            active_ = {};
        }

        Result<void> Resize(FramebufferExtent) override {
            return Result<void>::Success();
        }

        Result<bool> ObserveSubmission(const RenderCpuWait &w) override {
            return fixture_->graph.ObserveSubmission(w);
        }

        Result<void> BeginShutdown() override {
            return fixture_->graph.BeginShutdown();
        }

        Result<RenderBackendDrainProgress> PollDrain() override {
            auto drained = fixture_->graph.PollDrain();
            if (drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Drained) {
                if (releaseDrainedChildren_)
                    releaseDrainedChildren_();
                fixture_->resources.ShutdownResources();
            }
            return drained;
        }

    private:
        NativeGraphFixture *fixture_;
        std::function<void()> releaseDrainedChildren_;
        RenderBackendCapabilities capabilities_;
        FrameToken active_;
    };

    class ExternalGraphProvider final : public IRenderBackendProvider {
    public:
        explicit ExternalGraphProvider(NativeGraphFixture &fixture, RenderBackendId backend = RenderBackendId{"xr-bridge-test"},
                                       std::function<void()> releaseDrainedChildren = {})
            : fixture_(&fixture), backend_(std::move(backend)), releaseDrainedChildren_(std::move(releaseDrainedChildren)) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            return Result<std::unique_ptr<IRenderBackend>>::Success(
                std::make_unique<ExternalGraphBackend>(*fixture_, backend_, releaseDrainedChildren_));
        }

    private:
        NativeGraphFixture *fixture_;
        RenderBackendId backend_;
        std::function<void()> releaseDrainedChildren_;
    };

}  // namespace Horo::Render::VulkanExternalTest
