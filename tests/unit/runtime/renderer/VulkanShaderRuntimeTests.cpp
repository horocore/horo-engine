#include "../../../support/renderer/MultiviewTestSupport.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
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
    static_assert(!std::is_copy_constructible_v<VulkanPreparedMaterialBinding>);
    static_assert(!std::is_copy_assignable_v<VulkanPreparedMaterialBinding>);

    struct Fixture {
        Driver state;
        VulkanShaderRuntime runtime;
        std::uint64_t next{1};
        std::uint32_t maximumUniformDescriptors{128};
        std::uint32_t maximumStageUniformDescriptors{128};
        std::uint32_t maximumBoundSets{4};
        std::uint32_t maximumUniformRange{65536};
        std::vector<ShaderReflectedParameter> parameterOverride;
        bool pairedSampler{false};
        bool matrixParameters{false};
        std::size_t resolvedBufferBytes{16};
        std::uint64_t deviceIncarnation{};
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
            state.uniformRange = resolvedBufferBytes;
            VkPhysicalDeviceProperties properties{};
            properties.limits.minUniformBufferOffsetAlignment = 16;
            properties.limits.maxUniformBufferRange = maximumUniformRange;
            properties.limits.maxDescriptorSetUniformBuffers = maximumUniformDescriptors;
            properties.limits.maxDescriptorSetSampledImages = 128;
            properties.limits.maxDescriptorSetSamplers = 128;
            properties.limits.maxPerStageDescriptorUniformBuffers = maximumStageUniformDescriptors;
            properties.limits.maxPerStageDescriptorSampledImages = 128;
            properties.limits.maxPerStageDescriptorSamplers = 128;
            properties.limits.maxPerStageResources = 128;
            properties.limits.maxBoundDescriptorSets = maximumBoundSets;
            properties.limits.maxSamplerAllocationCount = 4096;
            return runtime.Initialize(Handle<VkDevice>(1), properties, RenderAdapterId{"vulkan:test"}, Resolve,
                                      [this]() -> std::optional<std::uint64_t> {
                return next++;
            }, [this](const MaterialResidentResource &r, ShaderResourceKind kind) -> Result<VulkanMaterialResource> {
                if (r.instance == 98 && kind == ShaderResourceKind::SampledTexture && r.kind == MaterialResidentResourceKind::TextureView &&
                    r.offset == 0 && r.byteCount == 0)
                    return Result<VulkanMaterialResource>::Success({.view = Handle<VkImageView>(98)});
                if (r.instance != 99 || kind != ShaderResourceKind::UniformBuffer || r.kind != MaterialResidentResourceKind::Buffer)
                    return Result<VulkanMaterialResource>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
                return Result<VulkanMaterialResource>::Success({.buffer = Handle<VkBuffer>(99), .bytes = resolvedBufferBytes});
            }, VulkanShaderRuntime::DeviceFeatures{deviceIncarnation, false});
        }

        void UseSampledMaterial() {
            manifest.bindings.push_back(
                {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            manifest.bindings.push_back(
                {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            pairedSampler = true;
        }

        void UseMatrixParameters() {
            matrixParameters = true;
            manifest.bindings[0].stages = ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment;
            manifest.parameters.push_back({{2}, {1}, ShaderValueType::Uint32, 1, 1, 1});
            manifest.parameters.push_back({{3}, {1}, ShaderValueType::Float32, 4, 4, 2});
            resolvedBufferBytes = 160;
        }

        void UseNativeViewTransforms() {
            UseMatrixParameters();
            manifest.parameters[2].arrayCount = 32;
            manifest.parameters.push_back({{4}, {1}, ShaderValueType::Float32, 4, 4, 1});
            parameterOverride = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1, 0, 0, 0, true, true},
                                 {{2}, {1}, ShaderValueType::Uint32, 1, 1, 1, 16, 0, 0, true, true},
                                 {{3}, {1}, ShaderValueType::Float32, 4, 4, 32, 32, 64, 16, true, true},
                                 {{4}, {1}, ShaderValueType::Float32, 4, 4, 1, 2080, 0, 16, true, true}};
            resolvedBufferBytes = 2144;
        }

        void UseMultiviewParameters() {
            manifest.bindings[0].stages = ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment;
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32},
                                   {{2}, {1}, ShaderValueType::Float32, 4, 4, 1},
                                   {{3}, {1}, ShaderValueType::Uint32, 1, 1, 32},
                                   {{4}, {1}, ShaderValueType::Uint32, 1, 1, 1}};
            parameterOverride = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 32, 0, 64, 16, true, true},
                                 {{2}, {1}, ShaderValueType::Float32, 4, 4, 1, 2048, 0, 16, true, true},
                                 {{3}, {1}, ShaderValueType::Uint32, 1, 1, 32, 2112, 16, 0, true, true},
                                 {{4}, {1}, ShaderValueType::Uint32, 1, 1, 1, 2624, 0, 0, true, true}};
            resolvedBufferBytes = 2640;
        }

        void Reflect() {
            ShaderReflectionCandidate candidate{.backend = ShaderTargetBackend::Vulkan,
                                                .interfaceSchemaVersion = 1,
                                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1, 0, 0, 0, true, true}}};
            if (matrixParameters) {
                candidate.stageInterface.push_back(
                    {ShaderStage::Vertex, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32, 1, 3, 1});
                candidate.parameters.push_back({{2}, {1}, ShaderValueType::Uint32, 1, 1, 1, 16, 0, 0, true, true});
                candidate.parameters.push_back({{3}, {1}, ShaderValueType::Float32, 4, 4, 2, 32, 64, 16, true, true});
            }
            if (!parameterOverride.empty())
                candidate.parameters = parameterOverride;
            for (const auto &binding : manifest.bindings) {
                candidate.bindings.push_back({binding.id, binding.kind, binding.access, binding.arrayCount, binding.stages, true});
                candidate.targetBindings.push_back({binding.id, 0, 0, binding.id.value - 1, "", {}, true});
            }
            if (pairedSampler)
                candidate.targetBindings[1].pairedSampler = {3};
            auto normalized = NormalizeShaderReflection(manifest, target, candidate);
            REQUIRE(normalized.HasValue());
            reflection = normalized.Value();
        }

        std::vector<std::uint8_t> Code(ShaderStage stage) {
            const std::array<std::uint32_t, 10>
                words{0x07230203, 0x00010600, 0, 2, 0, (5U << 16) | 15U, stage == ShaderStage::Vertex ? 0U : 4U, 1, 0x6e69616d, 0};
            std::vector<std::uint8_t> bytes(sizeof(words));
            std::ranges::transform(std::as_bytes(std::span{words}), bytes.begin(), [](std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
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
                    .resources = {{{1}, 0, MaterialParameterBinding{std::vector<std::byte>(resolvedBufferBytes)}}}};
        }
    };
}  // namespace

// VFX-native cases use the canonical fixture declared above; keep this dependent include after its definition.
#include "VulkanVfxShaderRuntimeTests.inl"

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
    std::move(table).Value().reset();
    CHECK(f.state.destroyedPools == 1);
    f.runtime.DestroyGraphicsPipeline(pipeline.Value());
    CHECK(f.state.destroyedPipelines == 1);
    f.runtime.Shutdown();
    CHECK(f.state.destroyedShaders == 2);
    f.runtime.Shutdown();
    CHECK(f.state.destroyedShaders == 2);
    CHECK(f.runtime.QueryPipelineDeviceIdentity().HasError());
}

TEST_CASE("Vulkan material matrix arrays use canonical reflected extents", "[vulkan][material][matrix]") {
    Fixture f;
    f.UseMatrixParameters();
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    auto material = f.Material();
    REQUIRE(f.runtime.QueryMaterialParameterBufferSize(pipeline.Value(), material, {1}).Value() == 160);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 160}};
    auto table = f.runtime.RealizeResolved(material, pipeline.Value(), resources);
    REQUIRE(table.HasValue());
    f.runtime.DestroyGraphicsPipeline(pipeline.Value());
    REQUIRE(f.state.destroyedPipelines == 0);
    std::move(table).Value().reset();
    REQUIRE(f.state.destroyedPipelines == 1);
}

TEST_CASE("Vulkan multiview uniforms admit 32 matrices object transform layers and exact padded tail", "[vulkan][material][matrix]") {
    Fixture f;
    f.UseMultiviewParameters();
    f.maximumUniformRange = 2640;
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    auto material = f.Material();
    SECTION("generic external uniform buffer") {
        material.resources[0].value = MaterialBufferBinding{{{1}, 6, 1}, 0, 2640};
    }
    SECTION("owned packed parameter block") {}
    REQUIRE(ComputeMaterialParameterBufferByteSize(material.layout, {1}).Value() == 2628);
    REQUIRE(f.runtime.QueryMaterialParameterBufferSize(pipeline.Value(), material, {1}).Value() == 2640);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 2640}};
    auto table = f.runtime.RealizeResolved(material, pipeline.Value(), resources);
    REQUIRE(table.HasValue());
    CHECK(f.state.updates == 1);
}

TEST_CASE("Vulkan downstream world object and layer block validates its exact final array tail", "[vulkan][material][matrix]") {
    Fixture f;
    f.UseMultiviewParameters();
    f.manifest.parameters.pop_back();
    f.parameterOverride.pop_back();
    f.resolvedBufferBytes = 2624;
    f.maximumUniformRange = 2624;
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    auto material = f.Material();
    material.resources[0].value = MaterialBufferBinding{{{1}, 6, 1}, 0, 2624};
    REQUIRE(ComputeMaterialParameterBufferByteSize(material.layout, {1}).Value() == 2612);
    REQUIRE(f.runtime.QueryMaterialParameterBufferSize(pipeline.Value(), material, {1}).Value() == 2624);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 2624}};
    REQUIRE(f.runtime.RealizeResolved(material, pipeline.Value(), resources).HasValue());
    CHECK(f.state.updates == 1);
}

TEST_CASE("Vulkan uniform bounds include reflected tail and native block padding", "[vulkan][material][matrix][limits]") {
    Fixture f;
    f.UseMultiviewParameters();
    f.maximumUniformRange = 2640;
    bool deviceTailRejected = false;
    SECTION("device limit fits reflected extent but not padded tail") {
        f.maximumUniformRange = 2639;
        deviceTailRejected = true;
    }
    SECTION("external buffer range omits final padding byte") {}
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    auto material = f.Material();
    material.resources[0].value = MaterialBufferBinding{{{1}, 6, 1}, 0, 2639};
    REQUIRE(ValidateMaterialBindingDescriptor(material, {1}).HasValue());
    CHECK(f.runtime.QueryMaterialParameterBufferSize(pipeline.Value(), material, {1}).HasError() == deviceTailRejected);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 2639}};
    REQUIRE(f.runtime.RealizeResolved(material, pipeline.Value(), resources).HasError());
    CHECK(f.state.pools == 0);
    CHECK(f.state.updates == 0);
}

TEST_CASE("Vulkan pipeline specialization overrides reach the declared native SpecId", "[vulkan][pipeline][specialization]") {
    Fixture f;
    f.Prepare();
    f.descriptor.specializationValues = {{{7}, ShaderValueType::Uint32, 13}};
    f.descriptor.compatibility.pipelineDescriptorDigest =
        ComputeRenderGraphicsPipelineDescriptorDigest(f.descriptor.state, f.descriptor.specializationValues).Value();
    REQUIRE(f.Pipeline().HasValue());
    CHECK(f.state.specialization == 13);
    f.descriptor.specializationValues[0].id = {8};
    f.descriptor.compatibility.pipelineDescriptorDigest =
        ComputeRenderGraphicsPipelineDescriptorDigest(f.descriptor.state, f.descriptor.specializationValues).Value();
    CHECK(f.runtime.QueryGraphicsPipelineMemoryCost(f.descriptor, f.vertex, f.fragment).HasError());
    CHECK(f.state.pipelines == 1);
}

TEST_CASE("Vulkan depth writes without testing use an always-pass native depth test", "[vulkan][pipeline][depth]") {
    Fixture f;
    f.Prepare();
    f.descriptor.state.depthTest = false;
    f.descriptor.state.depthWrite = true;
    f.descriptor.compatibility.pipelineDescriptorDigest = ComputeRenderGraphicsPipelineStateDigest(f.descriptor.state).Value();
    REQUIRE(f.Pipeline().HasValue());
    CHECK(f.state.depthTestEnabled);
    CHECK(f.state.depthWriteEnabled);
    CHECK(f.state.depthCompare == VK_COMPARE_OP_ALWAYS);
}

TEST_CASE("Vulkan realizes separate sampled views and samplers with logical pairing metadata", "[vulkan][material][sampled]") {
    Fixture f;
    f.UseSampledMaterial();
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    const auto cost = f.runtime.QuerySamplerMemoryCost({});
    REQUIRE(cost.HasValue());
    const auto sampler = f.runtime.CreateSampler({}, TestSupport::PlacementFor(cost.Value(), f.next));
    REQUIRE(sampler.HasValue());
    auto material = f.Material();
    material.resources.push_back({{2}, 0, RenderTextureViewHandle{{1}, 4, 1}});
    material.resources.push_back({{3}, 0, RenderSamplerHandle{{1}, 5, 1}});
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, 16},
                               MaterialResidentResource{MaterialResidentResourceKind::TextureView, 98, 0, 0},
                               MaterialResidentResource{MaterialResidentResourceKind::Sampler, sampler.Value(), 0, 0}};
    auto table = f.runtime.RealizeResolved(material, pipeline.Value(), resources);
    REQUIRE(table.HasValue());
    CHECK(f.state.updates == 1);
    std::move(table).Value().reset();
    f.runtime.DestroySampler(sampler.Value());
}

TEST_CASE("Vulkan rejects combined native coordinates before descriptor layout creation", "[vulkan][material][limits]") {
    Fixture f;
    f.UseSampledMaterial();
    f.Prepare();
    f.descriptor.reflection.targetBindings[2].nativeBinding = 1;
    const auto cost = f.runtime.QueryGraphicsPipelineMemoryCost(f.descriptor, f.vertex, f.fragment);
    REQUIRE(cost.HasError());
    CHECK(cost.ErrorValue().code.Value() == VulkanBackendErrors::ResourceUnsupported.code.Value());
    CHECK(f.state.layouts == 0);
    CHECK(f.state.pipelines == 0);
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

TEST_CASE("Vulkan device descriptor limits reject before layout creation", "[vulkan][pipeline][limits]") {
    Fixture f;
    SECTION("aggregate descriptor count") {
        f.maximumUniformDescriptors = 0;
    }
    SECTION("per-stage descriptor count") {
        f.maximumStageUniformDescriptors = 0;
    }
    SECTION("bound-set index") {
        f.maximumBoundSets = 0;
    }
    f.Prepare();
    const auto cost = f.runtime.QueryGraphicsPipelineMemoryCost(f.descriptor, f.vertex, f.fragment);
    REQUIRE(cost.HasError());
    CHECK(cost.ErrorValue().code.Value() == VulkanBackendErrors::ResourceUnsupported.code.Value());
    CHECK(f.state.layouts == 0);
    CHECK(f.state.pipelines == 0);
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
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasError());
    CHECK(pipeline.ErrorValue().message.find("VkResult") != std::string::npos);
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

TEST_CASE("Vulkan layer routing rejects mismatched cooked output and disabled device features", "[vulkan][shader][multiview]") {
    Fixture f;
    f.Prepare();
    SECTION("declared routing requires actual Layer output") {
        f.descriptor.state.viewRouting = RenderViewRouting::VertexLayer;
        f.descriptor.compatibility.pipelineDescriptorDigest =
            ComputeRenderGraphicsPipelineDescriptorDigest(f.descriptor.state, f.descriptor.specializationValues).Value();
        CHECK(f.runtime.QueryGraphicsPipelineMemoryCost(f.descriptor, f.vertex, f.fragment).HasError());
        CHECK(f.state.pipelines == 0);
    }
    SECTION("actual Layer output requires enabled device support") {
        auto bytes = f.Code(ShaderStage::Vertex);
        const std::array<std::uint32_t, 4> decorate{(4U << 16) | 71U, 1, 11, 9};
        for (auto byte : std::as_bytes(std::span{decorate}))
            bytes.push_back(std::to_integer<std::uint8_t>(byte));
        RenderShaderModuleDescriptor descriptor{f.target, ShaderStage::Vertex, "main", f.artifact, f.reflection.interfaceCompatibility};
        const auto cost = f.runtime.QueryShaderModuleMemoryCost(descriptor, bytes.size());
        REQUIRE(cost.HasValue());
        const auto calls = f.state.shaders;
        CHECK(f.runtime.CreateShaderModule(descriptor, bytes, TestSupport::PlacementFor(cost.Value(), 99)).HasError());
        CHECK(f.state.shaders == calls);
    }
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

#include "VulkanShaderNativeSmoke.inl"
