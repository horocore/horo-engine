#include "RenderMemoryTestSupport.h"
#include "VulkanShaderDispatchFixture.h"
#include "runtime/renderer/modules/vulkan/VulkanBackendInternal.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"
#include "runtime/renderer/modules/vulkan/VulkanShaderRuntime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    struct Fixture {
        Driver state;
        VulkanShaderRuntime runtime;
        std::uint64_t next{1};
        ShaderTargetRequirement target{.backend = ShaderTargetBackend::Vulkan,
                                       .payloadFormat = ShaderPayloadFormat::SpirV16,
                                       .descriptorVersion = 1,
                                       .interfaceSchemaVersion = 1,
                                       .maximumBindings = 16,
                                       .maximumInlineConstantBytes = 128};
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "tests/vulkan.hlsl",
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "main"}, {ShaderStage::Fragment, "main"}},
                                .bindings = {{{1},
                                              ShaderResourceKind::UniformBuffer,
                                              ShaderResourceAccess::ReadOnly,
                                              1,
                                              ShaderStageVisibility::Fragment}},
                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1}},
                                .specializationInputs = {{{7}, ShaderValueType::Uint32, 42, ShaderStageVisibility::Fragment}},
                                .targets = {target}};
        NormalizedShaderReflection reflection;
        Sha256Digest artifact{ComputeSha256(std::as_bytes(std::span{"artifact", 8}))};
        std::uint64_t vertex{}, fragment{}, pipeline{};
        RenderGraphicsPipelineDescriptor descriptor;

        Fixture() {
            driver = &state;
        }

        ~Fixture() {
            runtime.Shutdown();
            driver = nullptr;
        }

        Result<void> Initialize() {
            VkPhysicalDeviceProperties properties{};
            properties.limits.minUniformBufferOffsetAlignment = 16;
            properties.limits.maxUniformBufferRange = 65536;
            return runtime.Initialize(Handle<VkDevice>(1), properties, RenderAdapterId{"vulkan:test"}, Resolve,
                                      [this]() -> std::optional<std::uint64_t> {
                return next++;
            }, [](const MaterialResidentResource &r, ShaderResourceKind kind) -> Result<VulkanMaterialResource> {
                if (r.instance != 99 || kind != ShaderResourceKind::UniformBuffer || r.kind != MaterialResidentResourceKind::Buffer)
                    return Result<VulkanMaterialResource>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
                return Result<VulkanMaterialResource>::Success({.buffer = Handle<VkBuffer>(99), .bytes = 16});
            });
        }

        void Reflect() {
            ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Vulkan,
                                                .interfaceSchemaVersion = 1,
                                                .bindings = {{{1},
                                                              ShaderResourceKind::UniformBuffer,
                                                              ShaderResourceAccess::ReadOnly,
                                                              1,
                                                              ShaderStageVisibility::Fragment,
                                                              true}},
                                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1, 0, 0, 0, true, true}},
                                                .targetBindings = {{{1}, 0, 0, 0, "", {}, true}}};
            auto normalized = NormalizeShaderReflection(manifest, target, candidate);
            REQUIRE(normalized.HasValue());
            reflection = normalized.Value();
        }

        std::vector<std::uint8_t> Code(ShaderStage stage) {
            const std::array<std::uint32_t, 10>
                words{0x07230203, 0x00010600, 0, 2, 0, (5U << 16) | 15U, stage == ShaderStage::Vertex ? 0U : 4U, 1, 0x6e69616d, 0};
            std::vector<std::uint8_t> bytes(sizeof(words));
            std::memcpy(bytes.data(), words.data(), bytes.size());
            return bytes;
        }

        std::uint64_t Shader(ShaderStage stage) {
            const auto bytes = Code(stage);
            RenderShaderModuleDescriptor d{target, stage, "main", artifact, reflection.interfaceCompatibility};
            const auto cost = runtime.QueryShaderModuleMemoryCost(d, bytes.size());
            REQUIRE(cost.HasValue());
            const auto result = runtime.CreateShaderModule(d, bytes, TestSupport::PlacementFor(cost.Value(), next));
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void Prepare() {
            REQUIRE(Initialize().HasValue());
            Reflect();
            vertex = Shader(ShaderStage::Vertex);
            fragment = Shader(ShaderStage::Fragment);
            const auto identity = runtime.QueryPipelineDeviceIdentity();
            REQUIRE(identity.HasValue());
            descriptor = {.vertex = {{1}, 1, 1}, .fragment = {{1}, 2, 1}, .manifest = manifest, .target = target, .reflection = reflection};
            auto &c = descriptor.compatibility;
            c = {.schemaVersion = 1,
                 .backend = identity.Value().backend,
                 .backendModuleVersion = identity.Value().backendModuleVersion,
                 .adapter = identity.Value().adapter,
                 .deviceCompatibilityDigest = identity.Value().deviceCompatibilityDigest,
                 .driverIdentity = identity.Value().driverIdentity,
                 .driverVersion = identity.Value().driverVersion,
                 .shaderBackend = target.backend,
                 .shaderFormat = target.payloadFormat,
                 .shaderArtifactKey = artifact,
                 .shaderInterface = reflection.interfaceCompatibility,
                 .pipelineDescriptorDigest = ComputeRenderGraphicsPipelineStateDigest(descriptor.state).Value()};
        }

        Result<std::uint64_t> Pipeline() {
            auto cost = runtime.QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
            REQUIRE(cost.HasValue());
            return runtime.CreateGraphicsPipeline(descriptor, vertex, fragment, TestSupport::PlacementFor(cost.Value(), next));
        }

        MaterialBindingDescriptor Material() {
            auto layout = PrepareMaterialBindingLayout(reflection);
            REQUIRE(layout.HasValue());
            return {.material = {1},
                    .revision = 1,
                    .pipeline = {{1}, 3, 1},
                    .layout = layout.Value(),
                    .resources = {{{1}, 0, MaterialParameterBinding{std::vector<std::byte>(16)}}}};
        }
    };
}  // namespace

TEST_CASE("Vulkan realizes shaders pipeline specialization cache and immutable material sets", "[vulkan][shader][material]") {
    Fixture f;
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    CHECK(f.state.specialization == 42);
    auto material = f.Material();
    CHECK(f.runtime.QueryMaterialParameterBufferSize(pipeline.Value(), material, {1}).Value() == 16);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 16}};
    auto table = f.runtime.RealizeResolved(material, pipeline.Value(), resources);
    REQUIRE(table.HasValue());
    const auto *native = dynamic_cast<const VulkanPreparedMaterialBinding *>(table.Value().get());
    REQUIRE(native != nullptr);
    CHECK(native->pipeline != VK_NULL_HANDLE);
    CHECK(native->sets.size() == 1);
    CHECK(f.state.updates == 1);
    table.Value().reset();
    CHECK(f.state.destroyedPools == 1);
    f.runtime.DestroyGraphicsPipeline(pipeline.Value());
    CHECK(f.state.destroyedPipelines == 1);
    f.runtime.Shutdown();
    CHECK(f.state.destroyedShaders == 2);
    f.runtime.Shutdown();
    CHECK(f.state.destroyedShaders == 2);
    CHECK(f.runtime.QueryPipelineDeviceIdentity().HasError());
}

TEST_CASE("Vulkan shader admission rejects malformed artifacts before native creation", "[vulkan][shader][failure]") {
    Fixture f;
    REQUIRE(f.Initialize().HasValue());
    f.Reflect();
    auto code = f.Code(ShaderStage::Vertex);
    RenderShaderModuleDescriptor d{f.target, ShaderStage::Vertex, "main", f.artifact, f.reflection.interfaceCompatibility};
    auto cost = f.runtime.QueryShaderModuleMemoryCost(d, code.size());
    REQUIRE(cost.HasValue());
    SECTION("bad magic") {
        code[0] = 0;
    }
    SECTION("unterminated entry") {
        std::fill(code.end() - 4, code.end(), 1);
    }
    SECTION("zero instruction width") {
        code[22] = 0;
        code[23] = 0;
    }
    SECTION("mismatched stage") {
        d.stage = ShaderStage::Fragment;
    }
    SECTION("missing entry") {
        d.entryPoint = "missing";
    }
    CHECK(f.runtime.CreateShaderModule(d, code, TestSupport::PlacementFor(cost.Value(), 1)).HasError());
    CHECK(f.state.shaders == 0);
}

TEST_CASE("Vulkan pipeline native failures release partial native objects", "[vulkan][pipeline][failure]") {
    Fixture f;
    f.Prepare();
    SECTION("driver pipeline failure publishes partial handle") {
        f.state.pipelineResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    SECTION("layout failure") {
        f.state.layoutResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    CHECK(f.Pipeline().HasError());
    CHECK(f.state.destroyedPipelines == f.state.pipelines);
    if (f.state.layoutResult == VK_SUCCESS)
        CHECK(f.state.destroyedLayouts == f.state.layouts);
}

TEST_CASE("Vulkan material failure and incompatible cache inputs remain unpublished", "[vulkan][material][failure]") {
    Fixture f;
    f.Prepare();
    auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    auto material = f.Material();
    std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 16}};
    SECTION("foreign resource") {
        resources[0].instance = 777;
    }
    SECTION("misaligned range") {
        resources[0].offset = 1;
    }
    SECTION("short packed block") {
        resources[0].byteCount = 4;
    }
    SECTION("changed layout") {
        material.layout.parameters[0].byteOffset = 16;
    }
    SECTION("descriptor allocation failure") {
        f.state.allocationResult = VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    CHECK(f.runtime.RealizeResolved(material, pipeline.Value(), resources).HasError());
    CHECK(f.state.updates == 0);
    CHECK(f.state.destroyedPools == f.state.pools);
    f.descriptor.compatibility.driverVersion = "stale";
    CHECK(f.runtime.QueryGraphicsPipelineMemoryCost(f.descriptor, f.vertex, f.fragment).HasError());
}

TEST_CASE("Vulkan shader lifecycle rejects incomplete dispatch and foreign-thread admission", "[vulkan][shader][lifecycle]") {
    Fixture f;
    CHECK(f.runtime.QuerySamplerMemoryCost({}).HasError());
    f.state.missing = "vkCreatePipelineLayout";
    CHECK(f.Initialize().HasError());
    f.state.missing = {};
    REQUIRE(f.Initialize().HasValue());
    bool rejected = false;
    std::jthread worker([&] {
        rejected = f.runtime.QuerySamplerMemoryCost({}).HasError();
    });
    worker.join();
    CHECK(rejected);
    const auto cost = f.runtime.QuerySamplerMemoryCost({});
    REQUIRE(cost.HasValue());
    auto sampler = f.runtime.CreateSampler({}, TestSupport::PlacementFor(cost.Value(), 1));
    REQUIRE(sampler.HasValue());
    f.runtime.DestroySampler(sampler.Value());
    f.runtime.DestroySampler(sampler.Value());
}

#if defined(__linux__)
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
        std::uint64_t vertex{}, fragment{}, native{};

        void Initialize() {
            REQUIRE(RegisterVulkanRenderBackend(registry, loader).HasValue());
            auto created = registry.Create(RenderBackendId{"vulkan"});
            REQUIRE(created.HasValue());
            backend = std::move(created).Value();
            RenderBackendConfig config;
            config.requirePresentation = false;
            const auto initialized = backend->Initialize(config);
            INFO(initialized.HasError() ? initialized.ErrorValue().message : "Vulkan native device initialized");
            REQUIRE(initialized.HasValue());
        }

        std::uint64_t Shader(ShaderStage stage, const std::vector<std::uint8_t> &bytes) {
            RenderShaderModuleDescriptor d{metadata.target, stage, "main", metadata.artifact, metadata.reflection.interfaceCompatibility};
            auto cost = backend->QueryShaderModuleMemoryCost(d, bytes.size());
            REQUIRE(cost.HasValue());
            auto result = backend->CreateShaderModule(d, bytes, TestSupport::PlacementFor(cost.Value(), 1));
            INFO(result.HasError() ? result.ErrorValue().message : "Native shader created");
            REQUIRE(result.HasValue());
            return result.Value();
        }

        void PrepareShaders(const char *vertexPath, const char *fragmentPath) {
            metadata.Reflect();
            const auto vertexBytes = ReadShader(vertexPath), fragmentBytes = ReadShader(fragmentPath);
            std::vector<std::uint8_t> artifactBytes = vertexBytes;
            artifactBytes.insert(artifactBytes.end(), fragmentBytes.begin(), fragmentBytes.end());
            metadata.artifact = ComputeSha256(std::as_bytes(std::span{artifactBytes}));
            vertex = Shader(ShaderStage::Vertex, vertexBytes);
            fragment = Shader(ShaderStage::Fragment, fragmentBytes);
        }

        void PreparePipeline() {
            const auto identity = backend->QueryPipelineDeviceIdentity();
            REQUIRE(identity.HasValue());
            RenderGraphicsPipelineDescriptor pipeline{.vertex = {{1}, 1, 1},
                                                      .fragment = {{1}, 2, 1},
                                                      .manifest = metadata.manifest,
                                                      .target = metadata.target,
                                                      .reflection = metadata.reflection};
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
            const auto cost = backend->QueryGraphicsPipelineMemoryCost(pipeline, vertex, fragment);
            REQUIRE(cost.HasValue());
            const auto createdPipeline =
                backend->CreateGraphicsPipeline(pipeline, vertex, fragment, TestSupport::PlacementFor(cost.Value(), 2));
            INFO(createdPipeline.HasError() ? createdPipeline.ErrorValue().message : "Native graphics pipeline created");
            REQUIRE(createdPipeline.HasValue());
            native = createdPipeline.Value();
        }

        void PrepareMaterialAndShutdown() {
            const RenderBufferDescriptor bufferDescriptor{.byteSize = 16,
                                                          .usage = RenderBufferUsage::Uniform,
                                                          .access = RenderBufferAccess::HostVisible};
            const auto bufferCost = backend->QueryBufferMemoryCost(bufferDescriptor);
            REQUIRE(bufferCost.HasValue());
            const std::array<std::byte, 16> values{};
            const auto buffer = backend->CreateBuffer(bufferDescriptor, values, TestSupport::PlacementFor(bufferCost.Value(), 3));
            REQUIRE(buffer.HasValue());
            const auto material = metadata.Material();
            const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer.Value(), 0, 16}};
            auto table = backend->RealizeResolved(material, native, resources);
            INFO(table.HasError() ? table.ErrorValue().message : "Native descriptors updated");
            REQUIRE(table.HasValue());
            table.Value().reset();
            backend->DestroyBuffer(buffer.Value());
            backend->DestroyGraphicsPipeline(native);
            backend->DestroyShaderModule(vertex);
            backend->DestroyShaderModule(fragment);
            backend->Shutdown();
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

#endif
