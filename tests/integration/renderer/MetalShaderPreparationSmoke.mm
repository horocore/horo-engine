#include "../../support/renderer/RenderBackendDrainTestSupport.h"
#include "../../unit/runtime/renderer/RenderMemoryTestSupport.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "runtime/renderer/modules/metal/MetalBackendInternal.h"
#include "runtime/renderer/modules/metal/MetalPreparedMaterialState.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Owns a real native layer without acquiring drawables or creating a host window. */
        class PreparationLayerPort final : public IMetalPresentationPort {
        public:
            Result<void> CreateSurface() override {
                layer_ = [CAMetalLayer layer];
                return layer_ != nil ? Result<void>::Success()
                                     : Result<void>::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
            }

            void *Layer() const noexcept override {
                return (__bridge void *)layer_;
            }

            void DestroySurface() noexcept override {
                layer_ = nil;
            }

        private:
            __strong CAMetalLayer *layer_{nil};
        };

        /** @brief Preserves typed failures without asserting from a worker thread. */
        template <typename T> std::string PreparationFailureCode(const Result<T> &result) {
            return result.HasError() ? result.ErrorValue().code.Value() : "unexpected.success";
        }

        /** @brief Exercises every preparation admission without submitting frame work. */
        std::array<std::string, 9> PreparationFailureCodes(IMetalRuntime &runtime) {
            constexpr std::array<std::uint8_t, 4> bytes{};
            return {PreparationFailureCode(runtime.QueryPipelineDeviceIdentity()),
                    PreparationFailureCode(runtime.QueryShaderModuleMemoryCost({}, bytes.size())),
                    PreparationFailureCode(runtime.QueryGraphicsPipelineMemoryCost({}, 11, 13)),
                    PreparationFailureCode(runtime.QuerySamplerMemoryCost({})),
                    PreparationFailureCode(runtime.CreateShaderModule({}, bytes, {})),
                    PreparationFailureCode(runtime.CreateGraphicsPipeline({}, 17, 19, {})),
                    PreparationFailureCode(runtime.CreateSampler({}, {})),
                    PreparationFailureCode(runtime.QueryMaterialParameterBufferSize(23, {}, ShaderBindingId{1})),
                    PreparationFailureCode(runtime.RealizeResolved({}, 29, {}))};
        }

        /** @brief Keeps runtime destruction ahead of its borrowed bridge and native surface port. */
        struct NativeAdmissionFixture final {
            PreparationLayerPort port;
            MetalEditorGraphicsBridge bridge;
            std::unique_ptr<IMetalRuntime> runtime;

            void Initialize() {
                auto created = CreateMetalRuntime(port, bridge);
                REQUIRE(created.HasValue());
                runtime = std::move(created).Value();
                CHECK(port.Layer() == nullptr);
                CHECK(bridge.Device() == nullptr);
                for (const auto &code : PreparationFailureCodes(*runtime)) {
                    CHECK(code == MetalBackendErrors::NotInitialized.code.Value());
                }
                const auto admitted = runtime->Initialize({}, {});
                if (admitted.HasError() && admitted.ErrorValue().code.Value() == MetalBackendErrors::UnsupportedDeviceFamily.code.Value()) {
                    REQUIRE(bridge.Device() == nullptr);
                    REQUIRE(bridge.CommandQueue() == nullptr);
                    REQUIRE(bridge.CurrentCommandBuffer() == nullptr);
                    REQUIRE(port.Layer() == nullptr);
                    for (const auto &code : PreparationFailureCodes(*runtime)) {
                        REQUIRE(code == MetalBackendErrors::NotInitialized.code.Value());
                    }
                    SKIP("Native runtime lifecycle requires the Apple7 arm64 or Mac2 x86_64 baseline; " << admitted.ErrorValue().message);
                }
                INFO((admitted.HasError() ? admitted.ErrorValue().code.Value() + ": " + admitted.ErrorValue().message
                                          : "Native preparation runtime initialized."));
                REQUIRE(admitted.HasValue());
                REQUIRE(bridge.Device() != nullptr);
                REQUIRE(bridge.CommandQueue() != nullptr);
                CHECK(bridge.CurrentCommandBuffer() == nullptr);
                const auto identity = runtime->QueryPipelineDeviceIdentity();
                REQUIRE(identity.HasValue());
                CHECK(identity.Value().backend == RenderBackendId{"metal"});
            }

            void CheckForeignThreadAdmission() {
                std::array<std::string, 9> codes;
                std::exception_ptr unexpected;
                std::jthread worker([&] {
                    try {
                        codes = PreparationFailureCodes(*runtime);
                    } catch (...) {
                        unexpected = std::current_exception();
                    }
                });
                worker.join();
                if (unexpected) {
                    std::rethrow_exception(unexpected);
                }
                for (const auto &code : codes) {
                    CHECK(code == MetalBackendErrors::WrongThread.code.Value());
                }
            }

            void CheckOwnerSamplerDelegation() {
                const auto identity = runtime->QueryPipelineDeviceIdentity();
                REQUIRE(identity.HasValue());
                const auto cost = runtime->QuerySamplerMemoryCost({});
                REQUIRE(cost.HasValue());
                const auto sampler = runtime->CreateSampler({}, TestSupport::PlacementFor(cost.Value(), 1));
                REQUIRE(sampler.HasValue());
                runtime->DestroySampler(sampler.Value());
                CHECK(bridge.CurrentCommandBuffer() == nullptr);
            }

            void CheckOwnerShutdown() {
                Horo::Render::Test::RequireDrainedOwner(*runtime);
                CHECK(bridge.Device() == nullptr);
                CHECK(bridge.CommandQueue() == nullptr);
                CHECK(bridge.CurrentCommandBuffer() == nullptr);
                CHECK(port.Layer() == nullptr);
                for (const auto &code : PreparationFailureCodes(*runtime)) {
                    CHECK(code == MetalBackendErrors::NotInitialized.code.Value());
                }
            }
        };

        /** @brief Keeps each native preparation and rejection phase explicit without sharing mutable global state. */
        struct NativePreparationFixture final {
            __strong id<MTLDevice> device{nil};
            __strong id<MTLCommandQueue> queue{nil};
            MetalResourceRuntime runtime;
            RenderPipelineDeviceIdentity identity;
            std::vector<std::uint8_t> library;
            NormalizedShaderReflection reflection;
            Sha256Digest artifact;
            RenderGraphicsPipelineState state{};
            RenderGraphicsPipelineDescriptor pipeline;
            RenderMemoryCostPlan pipelineCost;
            MaterialBindingDescriptor material;
            std::array<MaterialResidentResource, 3> resolved;
            std::uint64_t vertex{}, fragment{}, native{}, buffer{}, sampler{}, texture{}, view{};
            const ShaderTargetRequirement target{.backend = ShaderTargetBackend::Metal,
                                                 .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                                                 .descriptorVersion = 1,
                                                 .interfaceSchemaVersion = 1,
                                                 .maximumBindings = 16,
                                                 .maximumInlineConstantBytes = 128};
            const ShaderManifest manifest{.schemaVersion = 1,
                                          .sourceIdentity = "tests/material.hlsl",
                                          .sourceRevision = 1,
                                          .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                                          .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer,
                                                        ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                                                       {ShaderBindingId{2}, ShaderResourceKind::SampledTexture,
                                                        ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                                                       {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                                                        ShaderStageVisibility::Fragment}},
                                          .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 1, 1}},
                                          .targets = {target}};
            ShaderReflectionCandidate
                reflectionCandidate{.backend = ShaderTargetBackend::Metal,
                                    .interfaceSchemaVersion = 1,
                                    .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                                                  ShaderStageVisibility::Fragment, true},
                                                 {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                                                  ShaderStageVisibility::Fragment, true},
                                                 {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                                                  ShaderStageVisibility::Fragment, true}},
                                    .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 1, 1, 0, 0, 0,
                                                    true, true}},
                                    .stageInterface = {{ShaderStage::Vertex, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32,
                                                        1, 3, 1, "position"},
                                                       {ShaderStage::Vertex, ShaderInterfaceDirection::Output, 0, ShaderValueType::Float32,
                                                        1, 2, 1, "uv"},
                                                       {ShaderStage::Fragment, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32,
                                                        1, 2, 1, "uv"},
                                                       {ShaderStage::Fragment, ShaderInterfaceDirection::Output, 0,
                                                        ShaderValueType::Float32, 1, 4, 1, "color"}},
                                    .targetBindings = {{ShaderBindingId{1}, 0, 0, 0, "material", {}, true},
                                                       {ShaderBindingId{2}, 0, 0, 1, "albedo", ShaderBindingId{3}, true},
                                                       {ShaderBindingId{3}, 0, 0, 1, "albedoSampler", {}, true}}};

            void Initialize() {
                device = MTLCreateSystemDefaultDevice();
                REQUIRE(device != nil);
                queue = [device newCommandQueue];
                REQUIRE(queue != nil);
                CHECK(runtime.QueryPipelineDeviceIdentity().HasError());
                runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
                const auto queried = runtime.QueryPipelineDeviceIdentity();
                REQUIRE(queried.HasValue());
                identity = queried.Value();
                CHECK(identity.backend == RenderBackendId{"metal"});
                std::ifstream input{HORO_METAL_PREPARATION_FIXTURE, std::ios::binary};
                REQUIRE(input.good());
                library = {std::istreambuf_iterator<char>{input}, {}};
                REQUIRE(library.size() >= 4);
            }

            void PrepareShaders() {
                const auto normalized = NormalizeShaderReflection(manifest, target, reflectionCandidate);
                REQUIRE(normalized.HasValue());
                reflection = normalized.Value();
                artifact = ComputeSha256(std::as_bytes(std::span{library}));
                RenderShaderModuleDescriptor module{target, ShaderStage::Vertex, "VertexMain", artifact, reflection.interfaceCompatibility};
                const auto vertexCost = runtime.QueryShaderModuleMemoryCost(module, library.size());
                REQUIRE(vertexCost.HasValue());
                const auto vertexPlacement = TestSupport::PlacementFor(vertexCost.Value(), 1);
                CHECK(runtime.QueryShaderModuleMemoryCost({}, 0).HasError());
                auto wrongPlacement = vertexPlacement;
                wrongPlacement.requiredBytes += 256;
                CHECK(runtime.CreateShaderModule(module, library, wrongPlacement).HasError());
                auto computeModule = module;
                computeModule.stage = ShaderStage::Compute;
                CHECK(runtime.QueryShaderModuleMemoryCost(computeModule, library.size()).HasError());
                auto malformed = library;
                malformed[0] = 0;
                CHECK(runtime.CreateShaderModule(module, malformed, vertexPlacement).HasError());
                const auto createdVertex = runtime.CreateShaderModule(module, library, vertexPlacement);
                REQUIRE(createdVertex.HasValue());
                vertex = createdVertex.Value();
                module.stage = ShaderStage::Fragment;
                module.entryPoint = "FragmentMain";
                const auto createdFragment = runtime.CreateShaderModule(module, library, TestSupport::PlacementFor(vertexCost.Value(), 2));
                REQUIRE(createdFragment.HasValue());
                fragment = createdFragment.Value();
                module.entryPoint = "Absent";
                CHECK(runtime.CreateShaderModule(module, library, TestSupport::PlacementFor(vertexCost.Value(), 3)).HasError());
            }

            void PreparePipeline() {
                state.depthFormat = RenderTextureFormat::Depth32Float;
                state.cull = RenderCullMode::Front;
                state.frontFace = RenderFrontFace::Clockwise;
                state.blend = RenderBlendMode::Alpha;
                const auto &actual = identity;
                pipeline = RenderGraphicsPipelineDescriptor{.vertex = {{1}, 1, 1},
                                                            .fragment = {{1}, 2, 1},
                                                            .manifest = manifest,
                                                            .target = target,
                                                            .reflection = reflection,
                                                            .state = state,
                                                            .compatibility = {.schemaVersion = 1,
                                                                              .backend = actual.backend,
                                                                              .backendModuleVersion = actual.backendModuleVersion,
                                                                              .adapter = actual.adapter,
                                                                              .deviceCompatibilityDigest = actual.deviceCompatibilityDigest,
                                                                              .driverIdentity = actual.driverIdentity,
                                                                              .driverVersion = actual.driverVersion,
                                                                              .shaderBackend = target.backend,
                                                                              .shaderFormat = target.payloadFormat,
                                                                              .shaderArtifactKey = artifact,
                                                                              .shaderInterface = reflection.interfaceCompatibility,
                                                                              .pipelineDescriptorDigest =
                                                                                  ComputeRenderGraphicsPipelineStateDigest(state).Value()}};
                const auto cost = runtime.QueryGraphicsPipelineMemoryCost(pipeline, vertex, fragment);
                REQUIRE(cost.HasValue());
                pipelineCost = cost.Value();
                const auto created = runtime.CreateGraphicsPipeline(pipeline, vertex, fragment, TestSupport::PlacementFor(cost.Value(), 4));
                INFO((created.HasError() ? created.ErrorValue().code.Value() + ": " + created.ErrorValue().message
                                         : std::string{"success"}));
                REQUIRE(created.HasValue());
                native = created.Value();
            }

            void CheckPipelineRejections() {
                auto incompatiblePacking = reflectionCandidate;
                incompatiblePacking.parameters[0].byteOffset = 4;
                const auto packing = NormalizeShaderReflection(manifest, target, incompatiblePacking);
                REQUIRE(packing.HasValue());
                auto incompatiblePipeline = pipeline;
                incompatiblePipeline.reflection = packing.Value();
                CHECK(runtime.CreateGraphicsPipeline(incompatiblePipeline, vertex, fragment, TestSupport::PlacementFor(pipelineCost, 40))
                          .HasError());
                auto multisample = pipeline;
                multisample.state.sampleCount = 2;
                multisample.compatibility.pipelineDescriptorDigest = ComputeRenderGraphicsPipelineStateDigest(multisample.state).Value();
                CHECK(runtime.QueryGraphicsPipelineMemoryCost(multisample, vertex, fragment).HasError());
                auto foreign = pipeline;
                foreign.compatibility.driverVersion += "foreign";
                CHECK(runtime.QueryGraphicsPipelineMemoryCost(foreign, vertex, fragment).HasError());
                CHECK(runtime.QueryGraphicsPipelineMemoryCost(pipeline, fragment, vertex).HasError());
            }

            void CheckReservedAndMissingBindings() {
                auto reserved = reflectionCandidate;
                reserved.targetBindings[0].nativeBinding = MetalMeshVertexBufferIndex;
                const auto reservedReflection = NormalizeShaderReflection(manifest, target, reserved);
                REQUIRE(reservedReflection.HasValue());
                auto reservedPipeline = pipeline;
                reservedPipeline.reflection = reservedReflection.Value();
                const auto rejected =
                    runtime.CreateGraphicsPipeline(reservedPipeline, vertex, fragment, TestSupport::PlacementFor(pipelineCost, 41));
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == MetalBackendErrors::UnsupportedResourceOperation.code.Value());
                auto missing = reflectionCandidate;
                missing.bindings[1].active = false;
                missing.targetBindings[1] = {ShaderBindingId{2}, 0, 0, 0, {}, {}, false};
                const auto missingReflection = NormalizeShaderReflection(manifest, target, missing);
                REQUIRE(missingReflection.HasValue());
                auto missingPipeline = pipeline;
                missingPipeline.reflection = missingReflection.Value();
                const auto extra =
                    runtime.CreateGraphicsPipeline(missingPipeline, vertex, fragment, TestSupport::PlacementFor(pipelineCost, 42));
                REQUIRE(extra.HasError());
                CHECK(extra.ErrorValue().code.Value() == MetalBackendErrors::ResourceIdentityInvalid.code.Value());
                CHECK(extra.ErrorValue().message.starts_with("Metal stage " +
                                                             std::to_string(static_cast<unsigned>(ShaderStageVisibility::Fragment)) + " "));
                CHECK(extra.ErrorValue().message.find("has 3 active material arguments; cooked reflection requires 2.") !=
                      std::string::npos);
            }

            void PrepareBufferAndSampler() {
                material = MaterialBindingDescriptor{.material = {1},
                                                     .revision = 1,
                                                     .pipeline = {{1}, 1, 1},
                                                     .layout = PrepareMaterialBindingLayout(reflection).Value()};
                const auto size = runtime.QueryMaterialParameterBufferSize(native, material, ShaderBindingId{1});
                REQUIRE(size.HasValue());
                CHECK(size.Value() == 16);
                CHECK(runtime.QueryMaterialParameterBufferSize(native, material, ShaderBindingId{2}).HasError());
                const RenderBufferDescriptor bufferDescriptor{.byteSize = 256, .usage = RenderBufferUsage::Uniform};
                const auto bufferCost = runtime.QueryBufferMemoryCost(bufferDescriptor);
                REQUIRE(bufferCost.HasValue());
                const auto createdBuffer = runtime.CreateBuffer(bufferDescriptor, {}, TestSupport::PlacementFor(bufferCost.Value(), 5));
                REQUIRE(createdBuffer.HasValue());
                buffer = createdBuffer.Value();
                RenderSamplerDescriptor unsupportedSampler;
                unsupportedSampler.maximumAnisotropy = 1.5F;
                CHECK(runtime.QuerySamplerMemoryCost(unsupportedSampler).HasError());
                unsupportedSampler.maximumAnisotropy = 1.0F;
                unsupportedSampler.compare = static_cast<RenderCompareFunction>(255);
                CHECK(runtime.QuerySamplerMemoryCost(unsupportedSampler).HasError());
                const auto samplerCost = runtime.QuerySamplerMemoryCost({});
                REQUIRE(samplerCost.HasValue());
                const auto createdSampler = runtime.CreateSampler({}, TestSupport::PlacementFor(samplerCost.Value(), 6));
                REQUIRE(createdSampler.HasValue());
                sampler = createdSampler.Value();
            }

            void PrepareTextureAndBindings() {
                const RenderTextureDescriptor textureDescriptor{.extent = {1, 1},
                                                                .format = RenderTextureFormat::Rgba8Unorm,
                                                                .usage = RenderTextureUsage::Sampled};
                const auto textureCost = runtime.QueryTextureMemoryCost(textureDescriptor);
                REQUIRE(textureCost.HasValue());
                const auto incompatibleTexture =
                    runtime.CreateTexture(textureDescriptor, {}, TestSupport::PlacementFor(textureCost.Value(), 7));
                REQUIRE(incompatibleTexture.HasError());
                CHECK(incompatibleTexture.ErrorValue().code.Value() == MetalBackendErrors::InvalidConfig.code.Value());
                CHECK(incompatibleTexture.ErrorValue().message == "Metal resource placement conflicts with its existing backing heap.");
                const auto createdTexture =
                    runtime.CreateTexture(textureDescriptor, {}, TestSupport::PlacementFor(textureCost.Value(), 8, 2));
                INFO((createdTexture.HasError() ? createdTexture.ErrorValue().code.Value() + ": " + createdTexture.ErrorValue().message
                                                : "Metal texture preparation succeeded."));
                REQUIRE(createdTexture.HasValue());
                texture = createdTexture.Value();
                const auto createdView = runtime.CreateTextureView({.texture = {{1}, 2, 1},
                                                                    .format = RenderTextureFormat::Rgba8Unorm,
                                                                    .aspect = RenderTextureAspect::Color},
                                                                   texture);
                REQUIRE(createdView.HasValue());
                view = createdView.Value();
                material.resources = {{ShaderBindingId{1}, 0, MaterialBufferBinding{.buffer = {{1}, 1, 1}, .offset = 0, .byteCount = 16}},
                                      {ShaderBindingId{2}, 0, RenderTextureViewHandle{{1}, 2, 1}},
                                      {ShaderBindingId{3}, 0, RenderSamplerHandle{{1}, 3, 1}}};
                resolved = std::array{MaterialResidentResource{MaterialResidentResourceKind::Buffer, buffer, 0, 16},
                                      MaterialResidentResource{MaterialResidentResourceKind::TextureView, view, 0, 0},
                                      MaterialResidentResource{MaterialResidentResourceKind::Sampler, sampler, 0, 0}};
            }

            void CheckMaterialAndRetainedLifetime() {
                auto table = runtime.RealizeResolved(material, native, resolved);
                REQUIRE(table.HasValue());
                auto incompatibleResources = resolved;
                incompatibleResources[0].offset = 1;
                CHECK(runtime.RealizeResolved(material, native, incompatibleResources).HasError());
                incompatibleResources = resolved;
                incompatibleResources[0].byteCount = 8;
                CHECK(runtime.RealizeResolved(material, native, incompatibleResources).HasError());
                incompatibleResources = resolved;
                incompatibleResources[1].kind = MaterialResidentResourceKind::Buffer;
                CHECK(runtime.RealizeResolved(material, native, incompatibleResources).HasError());
                incompatibleResources = resolved;
                incompatibleResources[2].instance = 0;
                CHECK(runtime.RealizeResolved(material, native, incompatibleResources).HasError());
                auto incompatibleMaterial = material;
                incompatibleMaterial.layout.parameters[0].byteOffset = 4;
                CHECK(runtime.QueryMaterialParameterBufferSize(native, incompatibleMaterial, ShaderBindingId{1}).HasError());
                CHECK(runtime.RealizeResolved(incompatibleMaterial, native, resolved).HasError());
                runtime.DestroyGraphicsPipeline(native);
                CHECK(runtime.QueryMaterialParameterBufferSize(native, material, ShaderBindingId{1}).HasError());
                auto ownedTable = std::move(table).Value();
                const auto *nativeTable = dynamic_cast<const MetalMaterialBinding *>(ownedTable.get());
                REQUIRE(nativeTable != nullptr);
                CHECK(nativeTable->state == state);
                ownedTable.reset();
            }

            void Shutdown() {
                runtime.DestroyTextureView(view);
                runtime.DestroyTexture(texture);
                runtime.DestroySampler(sampler);
                runtime.DestroySampler(sampler);
                runtime.DestroyBuffer(buffer);
                runtime.DestroyShaderModule(vertex);
                runtime.DestroyShaderModule(fragment);
                runtime.Shutdown();
                CHECK(runtime.QueryPipelineDeviceIdentity().HasError());
            }
        };
    }  // namespace

    TEST_CASE("Metal native preparation preserves owner and shutdown admission", "[gpu][metal]") {
        NativeAdmissionFixture fixture;
        fixture.Initialize();
        fixture.CheckForeignThreadAdmission();
        fixture.CheckOwnerSamplerDelegation();
        fixture.CheckOwnerShutdown();
    }

    TEST_CASE("Metal offline shader preparation verifies native ABI and exact material lifetime", "[gpu][metal]") {
        NativePreparationFixture fixture;
        fixture.Initialize();
        fixture.PrepareShaders();
        fixture.PreparePipeline();
        fixture.CheckPipelineRejections();
        fixture.CheckReservedAndMissingBindings();
        fixture.PrepareBufferAndSampler();
        fixture.PrepareTextureAndBindings();
        fixture.CheckMaterialAndRetainedLifetime();
        fixture.Shutdown();
    }
}  // namespace Horo::Render::Detail
