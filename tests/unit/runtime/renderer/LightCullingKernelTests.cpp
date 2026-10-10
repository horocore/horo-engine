#include "Horo/Runtime/Render/LightCullingKernel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Metadata-only cooked fixture; native loading remains a separate adapter obligation. */
    CookedLightCullingKernel Kernel() {
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
        kernel.artifact.payload = {'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        const std::array<std::uint8_t, 15> metadata{1,
                                                    0,
                                                    0,
                                                    0,
                                                    static_cast<std::uint8_t>(kernel.artifact.backend),
                                                    static_cast<std::uint8_t>(kernel.artifact.payloadFormat),
                                                    1,
                                                    0,
                                                    0,
                                                    0,
                                                    static_cast<std::uint8_t>(ShaderStage::Compute),
                                                    10,
                                                    0,
                                                    0,
                                                    0};
        kernel.artifact.payload.insert(kernel.artifact.payload.end(), metadata.begin(), metadata.end());
        constexpr std::array<std::uint8_t, 22> entry{'C', 'u', 'l', 'l', 'L', 'i', 'g', 'h', 't', 's', 4,
                                                     0,   0,   0,   0,   0,   0,   0,   'M', 'T', 'L', 'B'};
        kernel.artifact.payload.insert(kernel.artifact.payload.end(), entry.begin(), entry.end());
        kernel.artifact.artifactKey.bytes[0] = 1;
        kernel.reflection.backend = kernel.target.backend;
        kernel.reflection.interfaceSchemaVersion = 1;
        for (const auto &binding : kernel.manifest.bindings) {
            kernel.reflection.bindings.push_back({binding.id, binding.kind, binding.access, 1, ShaderStageVisibility::Compute, true});
            kernel.reflection.targetBindings.push_back({.id = binding.id, .nativeBinding = binding.id.value - 1});
        }
        for (const auto &parameter : kernel.manifest.parameters)
            kernel.reflection.parameters.push_back({.id = parameter.id,
                                                    .binding = parameter.binding,
                                                    .type = ShaderValueType::Uint32,
                                                    .byteOffset = (parameter.id.value - 1) * 4});
        return kernel;
    }
}  // namespace

TEST_CASE("Light kernel metadata admission preserves exact compute ABI and target map", "[runtime][renderer][light-kernel]") {
    const auto kernel = Kernel();
    const auto admitted = ValidateCookedLightCullingKernel(kernel, ShaderTargetBackend::Metal);
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().backend == ShaderTargetBackend::Metal);
    CHECK(admitted.Value().targetBindings.size() == 5);
    CHECK(admitted.Value().parameters.size() == 4);
    CHECK(ValidateCookedLightCullingKernel(kernel, ShaderTargetBackend::Vulkan).HasError());
}

TEST_CASE("Light kernel metadata rejects stage target layout and missing artifact evidence", "[runtime][renderer][light-kernel]") {
    auto kernel = Kernel();
    SECTION("Wrong entry") {
        kernel.manifest.entryPoints[0].name = "DifferentEntry";
    }
    SECTION("Wrong stage") {
        kernel.manifest.entryPoints[0].stage = ShaderStage::Fragment;
    }
    SECTION("Missing payload") {
        kernel.artifact.payload.clear();
    }
    SECTION("Missing key") {
        kernel.artifact.artifactKey = {};
    }
    SECTION("Wrong target format") {
        kernel.artifact.payloadFormat = ShaderPayloadFormat::SpirV16;
    }
    SECTION("Compute absent") {
        kernel.target.supportsCompute = false;
    }
    SECTION("Storage absent") {
        kernel.target.supportsStorageResources = false;
    }
    SECTION("Wrong packing") {
        kernel.reflection.parameters[1].byteOffset = 8;
    }
    SECTION("Missing resource") {
        kernel.reflection.targetBindings.pop_back();
    }
    SECTION("Native binding collision") {
        kernel.reflection.targetBindings[1].nativeBinding = 0;
    }
    SECTION("Wrong access") {
        kernel.manifest.bindings[2].access = ShaderResourceAccess::ReadOnly;
    }
    CHECK(ValidateCookedLightCullingKernel(kernel, ShaderTargetBackend::Metal).HasError());
}

TEST_CASE("Packed frame keeps 64-bit identities integer kinds and world-plane layout", "[runtime][renderer][light-kernel]") {
    const std::array lights{IdentifiedRenderLight{{0xABCDEF0012345678ULL}, {.kind = RenderLightKind::Spot, .position = {1, 2, 3}}}};
    LightCluster cluster;
    for (auto &plane : cluster.planes)
        plane = {{1, 0, 0}, 4};
    const std::array clusters{cluster};
    std::array<PackedRenderLight, 1> packedLights;
    std::array<PackedLightCluster, 1> packedClusters;
    REQUIRE(PackLightFrame(lights, clusters, packedLights, packedClusters).HasValue());
    CHECK(packedLights[0].identityLow == 0x12345678U);
    CHECK(packedLights[0].identityHigh == 0xABCDEF00U);
    CHECK(packedLights[0].kind == 2);
    CHECK((packedLights[0].positionRange == std::array<float, 4>{1, 2, 3, 10}));
    CHECK((packedClusters[0].planes[0] == std::array<float, 4>{1, 0, 0, 4}));
    CHECK(PackLightFrame(lights, clusters, {}, packedClusters).HasError());
    auto invalid = lights;
    invalid[0].identity.value = 0;
    CHECK(PackLightFrame(invalid, clusters, packedLights, packedClusters).HasError());
}
