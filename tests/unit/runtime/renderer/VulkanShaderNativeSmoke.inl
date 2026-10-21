#if defined(__linux__)
#include "../../../support/renderer/RenderBackendDrainTestSupport.h"

#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <iterator>

namespace {
    /** @brief Test-only system loader; no surface or display is required for native pipeline preparation. */
    class NativeLoader final : public IVulkanLoaderPort {
    public:
        ~NativeLoader() override {
            ReleaseLoader();
        }

        Result<void> AcquireLoader() override {
            library_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!library_)
                return Result<void>::Failure(MakeError(VulkanBackendErrors::NativeCallFailed, "System Vulkan loader missing."));
            return Result<void>::Success();
        }

        PFN_vkGetInstanceProcAddr GetInstanceProcAddr() noexcept override {
            return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_, "vkGetInstanceProcAddr"));
        }

        Result<std::vector<std::string>> RequiredPresentationExtensions() override {
            return Result<std::vector<std::string>>::Success({});
        }

        Result<bool> SupportsPresentation(VkPhysicalDevice, std::uint32_t) override {
            return Result<bool>::Success(false);
        }

        Result<bool> IsDriverAllowed(const VkPhysicalDeviceProperties &) override {
            return Result<bool>::Success(true);
        }

        void ReleaseLoader() noexcept override {
            if (library_)
                dlclose(library_);
            library_ = nullptr;
        }

    private:
        void *library_{};
    };

    std::vector<std::uint8_t> ReadShader(const char *path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), {}};
    }
}  // namespace

namespace {
    /** @brief Owns real native preparation in phases, releasing all consumer descriptors before device teardown. */
    struct NativePreparationFixture {
        NativeLoader loader;
        RenderBackendRegistry registry;
        std::unique_ptr<IRenderBackend> backend;
        Fixture metadata;
        std::uint64_t vertex{}, fragment{}, native{}, buffer{}, texture{}, view{}, sampler{};

        ~NativePreparationFixture() {
            if (!backend)
                return;
            const auto closing = backend->BeginShutdown();
            if (closing.HasError())
                std::terminate();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                const auto drained = backend->PollDrain();
                if (drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Drained)
                    break;
                if (std::chrono::steady_clock::now() >= deadline)
                    std::terminate();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        void Initialize() {
            REQUIRE(RegisterVulkanRenderBackend(registry, loader).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto created = registry.Create(RenderBackendId{"vulkan"});
            REQUIRE(created.HasValue());
            backend = std::move(created).Value();
            RenderBackendConfig config;
            config.requirePresentation = false;
            const auto initialized = backend->Initialize(config);
            INFO((initialized.HasError() ? initialized.ErrorValue().message : "Vulkan native device initialized"));
            REQUIRE(initialized.HasValue());
        }

        std::uint64_t Shader(ShaderStage stage, const std::vector<std::uint8_t> &bytes) {
            const auto entry = std::ranges::find(metadata.manifest.entryPoints, stage, &ShaderEntryPoint::stage);
            REQUIRE(entry != metadata.manifest.entryPoints.end());
            RenderShaderModuleDescriptor d{metadata.target, stage, entry->name, metadata.artifact,
                                           metadata.reflection.interfaceCompatibility};
            auto cost = backend->QueryShaderModuleMemoryCost(d, bytes.size());
            REQUIRE(cost.HasValue());
            auto result = backend->CreateShaderModule(d, bytes, TestSupport::PlacementFor(cost.Value(), 1));
            INFO((result.HasError() ? result.ErrorValue().message : "Native shader created"));
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void PrepareShaders(const char *vertexPath, const char *fragmentPath, bool sampled = true) {
            if (sampled)
                metadata.UseSampledMaterial();
            metadata.Reflect();
            const auto vertexBytes = ReadShader(vertexPath), fragmentBytes = ReadShader(fragmentPath);
            std::vector<std::uint8_t> artifactBytes = vertexBytes;
            artifactBytes.insert(artifactBytes.end(), fragmentBytes.begin(), fragmentBytes.end());
            metadata.artifact = ComputeSha256(std::as_bytes(std::span{artifactBytes}));
            vertex = Shader(ShaderStage::Vertex, vertexBytes);
            fragment = Shader(ShaderStage::Fragment, fragmentBytes);
        }

        void PreparePipeline(const RenderViewRouting routing = RenderViewRouting::SingleView) {
            const auto identity = backend->QueryPipelineDeviceIdentity();
            REQUIRE(identity.HasValue());
            RenderGraphicsPipelineDescriptor pipeline{.vertex = {{1}, 1, 1},
                                                      .fragment = {{1}, 2, 1},
                                                      .manifest = metadata.manifest,
                                                      .target = metadata.target,
                                                      .reflection = metadata.reflection};
            if (metadata.matrixParameters) {
                pipeline.state.depthFormat.reset();
                pipeline.state.depthTest = false;
                pipeline.state.depthWrite = false;
                pipeline.state.cull = RenderCullMode::None;
            }
            pipeline.state.viewRouting = routing;
            pipeline.compatibility = {.schemaVersion = 1,
                                      .backend = identity.Value().backend,
                                      .backendModuleVersion = identity.Value().backendModuleVersion,
                                      .adapter = identity.Value().adapter,
                                      .deviceCompatibilityDigest = identity.Value().deviceCompatibilityDigest,
                                      .driverIdentity = identity.Value().driverIdentity,
                                      .driverVersion = identity.Value().driverVersion,
                                      .shaderBackend = metadata.target.backend,
                                      .shaderFormat = metadata.target.payloadFormat,
                                      .shaderArtifactKey = metadata.artifact,
                                      .shaderInterface = metadata.reflection.interfaceCompatibility,
                                      .pipelineDescriptorDigest = ComputeRenderGraphicsPipelineStateDigest(pipeline.state).Value()};
            if (!metadata.manifest.specializationInputs.empty())
                pipeline.specializationValues = {{{7}, ShaderValueType::Uint32, 13}};
            pipeline.compatibility.pipelineDescriptorDigest =
                ComputeRenderGraphicsPipelineDescriptorDigest(pipeline.state, pipeline.specializationValues).Value();
            const auto cost = backend->QueryGraphicsPipelineMemoryCost(pipeline, vertex, fragment);
            REQUIRE(cost.HasValue());
            const auto createdPipeline =
                backend->CreateGraphicsPipeline(pipeline, vertex, fragment, TestSupport::PlacementFor(cost.Value(), 2));
            INFO((createdPipeline.HasError() ? createdPipeline.ErrorValue().message : "Native graphics pipeline created"));
            REQUIRE(createdPipeline.HasValue());
            native = createdPipeline.Value();
        }

        void PrepareUniformBuffer() {
            const RenderBufferDescriptor descriptor{.byteSize = 16,
                                                    .usage = RenderBufferUsage::Uniform,
                                                    .access = RenderBufferAccess::HostVisible};
            const auto cost = backend->QueryBufferMemoryCost(descriptor);
            REQUIRE(cost.HasValue());
            const std::array<std::byte, 16> values{};
            const auto created = backend->CreateBuffer(descriptor, values, TestSupport::PlacementFor(cost.Value(), 3));
            REQUIRE(created.HasValue());
            buffer = created.Value();
        }

        void PrepareSampledImage() {
            const RenderTextureDescriptor descriptor{.extent = {1, 1},
                                                     .format = RenderTextureFormat::Rgba8Unorm,
                                                     .usage = RenderTextureUsage::Sampled};
            const auto cost = backend->QueryTextureMemoryCost(descriptor);
            REQUIRE(cost.HasValue());
            const auto image = backend->CreateTexture(descriptor, {}, TestSupport::PlacementFor(cost.Value(), 4));
            REQUIRE(image.HasValue());
            texture = image.Value();
            const RenderTextureViewDescriptor viewDescriptor{.texture = {{1}, 4, 1}};
            const auto createdView = backend->CreateTextureView(viewDescriptor, texture);
            REQUIRE(createdView.HasValue());
            view = createdView.Value();
            const auto samplerCost = backend->QuerySamplerMemoryCost({});
            REQUIRE(samplerCost.HasValue());
            const auto createdSampler = backend->CreateSampler({}, TestSupport::PlacementFor(samplerCost.Value(), 5));
            REQUIRE(createdSampler.HasValue());
            sampler = createdSampler.Value();
        }

        void PrepareMaterialAndShutdown() {
            PrepareUniformBuffer();
            PrepareSampledImage();
            auto material = metadata.Material();
            material.resources.push_back({{2}, 0, RenderTextureViewHandle{{1}, 4, 1}});
            material.resources.push_back({{3}, 0, RenderSamplerHandle{{1}, 5, 1}});
            const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer, 0, 16},
                                       MaterialResidentResource{MaterialResidentResourceKind::TextureView, view, 0, 0},
                                       MaterialResidentResource{MaterialResidentResourceKind::Sampler, sampler, 0, 0}};
            auto table = backend->RealizeResolved(material, native, resources);
            INFO((table.HasError() ? table.ErrorValue().message : "Native descriptors updated"));
            REQUIRE(table.HasValue());
            std::move(table).Value().reset();
            backend->DestroyBuffer(buffer);
            backend->DestroyTextureView(view);
            backend->DestroyTexture(texture);
            backend->DestroySampler(sampler);
            backend->DestroyGraphicsPipeline(native);
            backend->DestroyShaderModule(vertex);
            backend->DestroyShaderModule(fragment);
            Test::RequireDrainedOwner(*backend);
            CHECK(backend->QueryPipelineDeviceIdentity().HasError());
        }
    };
}  // namespace

TEST_CASE("Vulkan native device consumes compiled SPIR-V pipeline and material descriptors", "[vulkan][shader][native-smoke]") {
    const char *vertexPath = std::getenv("HORO_VULKAN_VERTEX_FIXTURE");
    const char *fragmentPath = std::getenv("HORO_VULKAN_FRAGMENT_FIXTURE");
    if (!vertexPath || !fragmentPath)
        SKIP("Opt-in native qualification requires both compiled SPIR-V fixture paths.");
    NativePreparationFixture fixture;
    fixture.Initialize();
    fixture.PrepareShaders(vertexPath, fragmentPath);
    fixture.PreparePipeline();
    fixture.PrepareMaterialAndShutdown();
}

#include "VulkanTransformNativeSmoke.inl"

// Multiview native qualification borrows the transform fixture defined above.
#include "VulkanMultiviewNativeSmoke.inl"

#endif
