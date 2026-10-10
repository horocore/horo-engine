#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalResourceInstances.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Loads a toolchain-cooked HOROSHDR package and its authoritative reflected Metal buffer indices. */
        CookedLightCullingKernel CookedKernel(const char *path, std::string_view mapping) {
            CookedLightCullingKernel kernel;
            kernel.target = {.backend = ShaderTargetBackend::Metal,
                             .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                             .descriptorVersion = 1,
                             .interfaceSchemaVersion = 1,
                             .maximumBindings = 5,
                             .maximumInlineConstantBytes = 0,
                             .supportsCompute = true,
                             .supportsStorageResources = true};
            kernel.manifest = MakeLightCullingShaderManifest(kernel.target);
            kernel.artifact.backend = kernel.target.backend;
            kernel.artifact.payloadFormat = kernel.target.payloadFormat;
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            REQUIRE(input.good());
            const auto size = input.tellg();
            REQUIRE(size > 0);
            REQUIRE(size <= 16 * 1024 * 1024);
            input.seekg(0);
            kernel.artifact.payload.resize(static_cast<std::size_t>(size));
            input.read(reinterpret_cast<char *>(kernel.artifact.payload.data()), size);
            REQUIRE(input.good());
            // The runtime admission requires an artifact identity; actual native reflection independently validates this test input.
            kernel.artifact.artifactKey.bytes[0] = 1;
            kernel.reflection.backend = kernel.target.backend;
            kernel.reflection.interfaceSchemaVersion = 1;
            for (const auto &binding : kernel.manifest.bindings) {
                std::uint32_t index{};
                const auto comma = mapping.find(',');
                const auto token = mapping.substr(0, comma);
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), index);
                REQUIRE(parsed.ec == std::errc{});
                REQUIRE(parsed.ptr == token.data() + token.size());
                kernel.reflection.bindings.push_back({binding.id, binding.kind, binding.access, 1, ShaderStageVisibility::Compute, true});
                kernel.reflection.targetBindings.push_back({.id = binding.id, .nativeBinding = index});
                if (binding.id.value != 5) {
                    REQUIRE(comma != std::string_view::npos);
                    mapping.remove_prefix(comma + 1);
                } else {
                    REQUIRE(comma == std::string_view::npos);
                }
            }
            for (const auto &parameter : kernel.manifest.parameters)
                kernel.reflection.parameters.push_back({.id = parameter.id,
                                                        .binding = parameter.binding,
                                                        .type = ShaderValueType::Uint32,
                                                        .byteOffset = (parameter.id.value - 1) * 4});
            return kernel;
        }

        /** @brief Supplies deterministic CPU expectations for boundary, spot and overflow cases. */
        struct CullingFrame final {
            LightCullingBudget budget{.maximumLights = 4,
                                      .maximumClusters = 2,
                                      .referencesPerCluster = 2,
                                      .requireCompleteCoverage = false};
            std::array<IdentifiedRenderLight, 4> lights{{{{1}, {.kind = RenderLightKind::Directional}},
                                                         {{2}, {.kind = RenderLightKind::Point, .position = {2, 0, 0}, .range = 1}},
                                                         {{3}, {.kind = RenderLightKind::Spot, .position = {0, 0, 0}, .range = 1}},
                                                         {{4}, {.kind = RenderLightKind::Point, .position = {100, 0, 0}, .range = 1}}}};
            std::array<LightCluster, 2> clusters{};
            std::array<PackedRenderLight, 4> packedLights{};
            std::array<PackedLightCluster, 2> packedClusters{};
            std::array<LightClusterMembership, 2> membership{};
            std::array<std::uint32_t, 4> references{};

            CullingFrame() {
                const std::array<Math::Vec3, 6> normals{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
                for (std::size_t cluster = 0; cluster < clusters.size(); ++cluster)
                    for (std::size_t plane = 0; plane < normals.size(); ++plane)
                        clusters[cluster].planes[plane] = {normals[plane], cluster == 0 ? 1.0F : 0.5F};
                REQUIRE(PackLightFrame(lights, clusters, packedLights, packedClusters).HasValue());
                REQUIRE(CullLightsCpu(lights, clusters, budget, membership, references).HasValue());
            }

            LightFrameUpdate Update() const {
                return {.budget = budget, .revision = 1, .lights = packedLights, .clusters = packedClusters};
            }
        };

        /** @brief Creates the same bounded native tables for each runtime under test. */
        void CreateFrameBuffers(MetalResourceRuntime &runtime, const CullingFrame &frame, std::array<std::uint64_t, 4> &instances,
                                std::array<RenderGraphResourceInstance, 4> &resources) {
            const std::array<std::size_t, 4> sizes{sizeof(frame.packedLights), sizeof(frame.packedClusters),
                                                   frame.clusters.size() * sizeof(PackedLightMembership), sizeof(frame.references)};
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                const RenderBufferDescriptor descriptor{.byteSize = sizes[index],
                                                        .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource,
                                                        .access = RenderBufferAccess::HostVisible};
                const auto cost = runtime.QueryBufferMemoryCost(descriptor);
                REQUIRE(cost.HasValue());
                const auto created = runtime.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost.Value(), index + 1, index + 1));
                REQUIRE(created.HasValue());
                instances[index] = created.Value();
                resources[index] = {{{1}, static_cast<std::uint32_t>(index + 1)}, instances[index]};
            }
            REQUIRE(runtime.UpdateLightFrame({instances, frame.Update()}).HasValue());
        }

        /** @brief Compares only completed GPU outputs against the bounded CPU recipe. */
        void CheckMembership(const CullingFrame &frame, const std::array<std::uint64_t, 4> &instances) {
            const auto *membershipBuffer = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(instances[2]));
            const auto *referenceBuffer = reinterpret_cast<MetalBufferInstance *>(static_cast<std::uintptr_t>(instances[3]));
            const auto *actual = static_cast<const PackedLightMembership *>(membershipBuffer->buffer.contents);
            const auto *references = static_cast<const std::uint32_t *>(referenceBuffer->buffer.contents);
            for (std::size_t cluster = 0; cluster < frame.clusters.size(); ++cluster) {
                CHECK(actual[cluster].offset == frame.membership[cluster].offset);
                CHECK(actual[cluster].count == frame.membership[cluster].count);
                CHECK(actual[cluster].omitted == frame.membership[cluster].omitted);
                CHECK(actual[cluster].reserved == 0);
                for (std::size_t index = 0; index < frame.membership[cluster].count; ++index)
                    CHECK(references[actual[cluster].offset + index] == frame.references[frame.membership[cluster].offset + index]);
            }
        }

        /** @brief Verifies stale runtime ownership using live replacement buffers and an otherwise valid revision. */
        void CheckReplacementOwner(const CookedLightCullingKernel &cooked, const CullingFrame &frame, id<MTLDevice> device,
                                   id<MTLCommandQueue> queue, RenderGraphWorkload &workload) {
            MetalResourceRuntime replacement;
            replacement.Initialize((__bridge void *)device, (__bridge void *)queue);
            std::array<std::uint64_t, 4> instances{};
            std::array<RenderGraphResourceInstance, 4> resources{};
            CreateFrameBuffers(replacement, frame, instances, resources);
            const auto staleOwner = replacement.ValidateGraphWorkload(workload, resources);
            REQUIRE(staleOwner.HasError());
            CHECK(staleOwner.ErrorValue().code.Value() == LightCullingErrors::InvalidInput.code.Value());
            const auto kernel = replacement.RealizeLightCullingKernel(cooked);
            REQUIRE(kernel.HasValue());
            std::get<RenderGraphLightCulling>(workload).kernel = kernel.Value();
            REQUIRE(replacement.ValidateGraphWorkload(workload, resources).HasValue());
            replacement.Shutdown();
            std::get<RenderGraphLightCulling>(workload).kernel.reset();
        }

        /** @brief Waits only within the smoke-test deadline for actual command completion. */
        void RequireCompletion(id<MTLCommandBuffer> commands) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
        }
    }  // namespace

    TEST_CASE("Cooked Metal light culling matches bounded CPU membership and overflow", "[integration][renderer][metal][gpu][light]") {
        const char *package = std::getenv("HORO_LIGHT_CULLING_PACKAGE");
        const char *mapping = std::getenv("HORO_LIGHT_CULLING_METAL_BINDINGS");
        if (package == nullptr || mapping == nullptr)
            SKIP("Supply a locked-toolchain HOROSHDR package and reflected lights,clusters,membership,references,dispatch Metal indices.");
        const auto cooked = CookedKernel(package, mapping);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        REQUIRE(device != nil);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        REQUIRE(queue != nil);
        MetalResourceRuntime runtime;
        runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
        auto kernel = runtime.RealizeLightCullingKernel(cooked);
        REQUIRE(kernel.HasValue());
        const CullingFrame frame;
        std::array<std::uint64_t, 4> instances{};
        std::array<RenderGraphResourceInstance, 4> resources{};
        CreateFrameBuffers(runtime, frame, instances, resources);
        RenderGraphWorkload workload = RenderGraphLightCulling{resources[0].resource, resources[1].resource, resources[2].resource,
                                                               resources[3].resource, {4, 2, 2, 0},          1,
                                                               kernel.Value()};
        REQUIRE(runtime.ValidateGraphWorkload(workload, resources).HasValue());
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        REQUIRE(runtime.ExecuteGraphWorkload((__bridge void *)commands, workload, resources).HasValue());
        [commands commit];
        RequireCompletion(commands);
        CheckMembership(frame, instances);
        runtime.Shutdown();
        CHECK(runtime.ValidateGraphWorkload(workload, resources).HasError());
        CheckReplacementOwner(cooked, frame, device, queue, workload);
    }
}  // namespace Horo::Render::Detail
