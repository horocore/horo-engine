#include "../../unit/runtime/renderer/RenderMemoryTestSupport.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "LockedVfxShaderFixture.h"
#include "runtime/renderer/modules/metal/MetalResourceRuntime.h"

#import <Metal/Metal.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Reads only the bounded library built from a captured stage by this target's native fixture dependency. */
        std::vector<std::uint8_t> Library(const std::string &stage) {
            const auto path = std::filesystem::path{HORO_VFX_METAL_FIXTURE_DIRECTORY} / ("VfxLocked-" + stage + ".metallib");
            REQUIRE_FALSE(std::filesystem::is_symlink(path));
            REQUIRE(std::filesystem::is_regular_file(path));
            const auto size = std::filesystem::file_size(path);
            REQUIRE(size >= 4);
            REQUIRE(size <= 8U * 1024U * 1024U);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(input.good());
            REQUIRE((std::array{bytes[0], bytes[1], bytes[2], bytes[3]} == std::array<std::uint8_t, 4>{'M', 'T', 'L', 'B'}));
            return bytes;
        }

        /** @brief Exercises the actual private native preparation owner without advertising frontend VFX capability. */
        struct VfxPreparationFixture {
            __strong id<MTLDevice> device{nil};
            __strong id<MTLCommandQueue> queue{nil};
            MetalResourceRuntime runtime;
            Tests::LockedVfxShaderEvidence evidence;
            RenderGraphicsPipelineDescriptor descriptor;
            std::uint64_t vertex{}, fragment{}, pipeline{};
            std::uint64_t nextPlacement{10};

            explicit VfxPreparationFixture(const bool masked)
                : evidence(Tests::LoadLockedVfxShaderEvidence(ShaderTargetBackend::Metal, masked)) {
                device = MTLCreateSystemDefaultDevice();
                REQUIRE(device != nil);
                queue = [device newCommandQueue];
                REQUIRE(queue != nil);
                runtime.Initialize((__bridge void *)device, (__bridge void *)queue);
                auto reflection = NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
                INFO((reflection.HasError() ? reflection.ErrorValue().message : "Actual captured Metal reflection admitted"));
                REQUIRE(reflection.HasValue());
                const auto identity = runtime.QueryPipelineDeviceIdentity();
                REQUIRE(identity.HasValue());
                const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VSMain", Library("vs")},
                                        ShaderArtifactStage{ShaderStage::Fragment, "PSMain", Library(masked ? "masked-ps" : "ps")}};
                const auto package =
                    PackageShaderArtifactStages(evidence.target.backend, evidence.target.payloadFormat, stages, 16U * 1024U * 1024U);
                REQUIRE(package.HasValue());
                const auto artifact = ComputeSha256(std::as_bytes(std::span{package.Value()}));
                vertex = Module(stages[0], artifact, reflection.Value());
                fragment = Module(stages[1], artifact, reflection.Value());
                RenderGraphicsPipelineState state;
                state.colorFormat = RenderTextureFormat::Rgba16Float;
                state.depthFormat = RenderTextureFormat::Depth32Float;
                const auto digest = ComputeRenderGraphicsPipelineStateDigest(state);
                REQUIRE(digest.HasValue());
                const auto &actual = identity.Value();
                const auto shaderInterface = reflection.Value().interfaceCompatibility;
                descriptor = {.vertex = {{1}, 1, 1},
                              .fragment = {{1}, 2, 1},
                              .manifest = evidence.manifest,
                              .target = evidence.target,
                              .reflection = std::move(reflection).Value(),
                              .state = state,
                              .compatibility = {.schemaVersion = 1,
                                                .backend = actual.backend,
                                                .backendModuleVersion = actual.backendModuleVersion,
                                                .adapter = actual.adapter,
                                                .deviceCompatibilityDigest = actual.deviceCompatibilityDigest,
                                                .driverIdentity = actual.driverIdentity,
                                                .driverVersion = actual.driverVersion,
                                                .shaderBackend = evidence.target.backend,
                                                .shaderFormat = evidence.target.payloadFormat,
                                                .shaderArtifactKey = artifact,
                                                .shaderInterface = shaderInterface,
                                                .pipelineDescriptorDigest = digest.Value()}};
            }

            /** @brief Creates one exact captured native stage through the same preparation owner and placement ledger. */
            std::uint64_t Module(const ShaderArtifactStage &stage, const Sha256Digest &artifact,
                                 const NormalizedShaderReflection &reflection) {
                const RenderShaderModuleDescriptor description{evidence.target, stage.stage, stage.entryPoint, artifact,
                                                               reflection.interfaceCompatibility};
                // The frontend unwraps the stage package before calling this private native owner. Preserve the
                // package identity while passing only the exact captured stage's real MTLB bytes here too.
                const auto cost = runtime.QueryShaderModuleMemoryCost(description, stage.bytes.size());
                INFO((cost.HasError() ? cost.ErrorValue().message : "Native stage allocation admitted"));
                REQUIRE(cost.HasValue());
                const auto prepared =
                    runtime.CreateShaderModule(description, stage.bytes, TestSupport::PlacementFor(cost.Value(), nextPlacement++));
                INFO((prepared.HasError() ? prepared.ErrorValue().message : "Exact captured native stage prepared"));
                REQUIRE(prepared.HasValue());
                return prepared.Value();
            }

            ~VfxPreparationFixture() {
                if (pipeline)
                    runtime.DestroyGraphicsPipeline(pipeline);
                if (fragment)
                    runtime.DestroyShaderModule(fragment);
                if (vertex)
                    runtime.DestroyShaderModule(vertex);
                // No commands were submitted by preparation-only cases; the ordinary owner teardown remains authoritative.
                runtime.Shutdown();
            }

            /** @brief Publishes only a pipeline whose actual Metal reflection passes native preparation. */
            void Prepare() {
                const auto cost = runtime.QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
                REQUIRE(cost.HasValue());
                const auto prepared =
                    runtime.CreateGraphicsPipeline(descriptor, vertex, fragment, TestSupport::PlacementFor(cost.Value(), nextPlacement++));
                INFO((prepared.HasError() ? prepared.ErrorValue().message : "actual native VFX preparation succeeded"));
                REQUIRE(prepared.HasValue());
                pipeline = prepared.Value();
            }

            /** @brief Requires rejection while reclaiming any unexpected successful native pipeline before assertion. */
            void Reject(const RenderGraphicsPipelineDescriptor &candidate) {
                const auto cost = runtime.QueryGraphicsPipelineMemoryCost(candidate, vertex, fragment);
                if (cost.HasError())
                    return;
                const auto rejected =
                    runtime.CreateGraphicsPipeline(candidate, vertex, fragment, TestSupport::PlacementFor(cost.Value(), nextPlacement++));
                if (rejected.HasValue())
                    runtime.DestroyGraphicsPipeline(rejected.Value());
                CHECK(rejected.HasError());
            }
        };
    }  // namespace

    TEST_CASE("Captured VFX Metal stages validate actual array ABI and report padded material extent", "[gpu][metal][vfx][shader]") {
        VfxPreparationFixture fixture{GENERATE(false, true)};
        fixture.Prepare();
        auto layout = PrepareMaterialBindingLayout(fixture.descriptor.reflection);
        REQUIRE(layout.HasValue());
        const MaterialBindingDescriptor material{.layout = std::move(layout).Value()};
        const auto instances =
            fixture.runtime.QueryMaterialParameterBufferSize(fixture.pipeline, material, VfxShaderParameterIds::Instances);
        REQUIRE(instances.HasValue());
        CHECK(instances.Value() == 10240);
        const auto materialBytes =
            fixture.runtime.QueryMaterialParameterBufferSize(fixture.pipeline, material, VfxShaderParameterIds::Material);
        REQUIRE(materialBytes.HasValue());
        CHECK(materialBytes.Value() >= 20);
        CHECK(materialBytes.Value() <= 32);
        // Consumers allocate this queried native extent, never the logical last-member end of 20.
    }

    TEST_CASE("Captured VFX Metal native reflection rejects incompatible instance packing", "[gpu][metal][vfx][shader]") {
        VfxPreparationFixture fixture{true};
        fixture.Prepare();
        auto candidate = fixture.descriptor;
        auto &matrix = candidate.reflection.parameters[0];
        SECTION("Matrix shape") {
            matrix.rows = 3;
        }
        SECTION("Array count") {
            matrix.arrayCount = 127;
        }
        SECTION("Array stride") {
            matrix.arrayStride = 128;
        }
        SECTION("Matrix stride") {
            matrix.matrixStride = 32;
        }
        SECTION("Matrix order") {
            matrix.columnMajor = false;
        }
        SECTION("Member offset") {
            matrix.byteOffset = 16;
        }
        SECTION("Color offset") {
            candidate.reflection.parameters[1].byteOffset = 8208;
        }
        SECTION("Foreign interface cannot opt into native arrays") {
            candidate.reflection.interfaceCompatibility = {};
        }
        fixture.Reject(candidate);
    }
}  // namespace Horo::Render::Detail
