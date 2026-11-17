#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "VfxGpuVulkanReflection.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#ifndef HORO_VFX_GPU_REFLECTION_FIXTURE_PATH
#error "Supply the immutable actual captured SPIR-V/JSON fixture directory."
#endif
namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr auto Role = VfxGpuProgramRole::Simulation;

    std::vector<std::uint8_t> Read(std::string_view name) {
        std::ifstream input(std::filesystem::path{HORO_VFX_GPU_REFLECTION_FIXTURE_PATH} / name, std::ios::binary | std::ios::ate);
        REQUIRE(input.good());
        const auto size = input.tellg();
        REQUIRE(size > 0);
        REQUIRE(size <= 1024 * 1024);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(input.good());
        return bytes;
    }

    ShaderTargetRequirement Target() {
        return {.backend = ShaderTargetBackend::Vulkan,
                .payloadFormat = ShaderPayloadFormat::SpirV16,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 32,
                .maximumInlineConstantBytes = 128,
                .supportsCompute = true,
                .supportsStorageResources = true};
    }

    struct Capture {
        std::vector<std::uint8_t> spirv{Read("compute.spv")};
        std::vector<std::uint8_t> json{Read("compute.json")};
        ShaderManifest manifest{MakeVfxGpuShaderManifest(Role, Target())};
    };

    Result<NormalizedShaderReflection> Normalize(const Capture &capture, std::string json) {
        const std::array stages{ShaderArtifactStage{ShaderStage::Compute, "SimulateParticles", capture.spirv}};
        auto payload = PackageShaderArtifactStages(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, stages, 1024 * 1024);
        REQUIRE(payload.HasValue());
        CompiledShaderArtifact compiled{.backend = ShaderTargetBackend::Vulkan,
                                        .payloadFormat = ShaderPayloadFormat::SpirV16,
                                        .payload = std::move(payload).Value()};
        const std::array<SpirvReflectionStage, 1> reflection{{{ShaderStage::Compute, "SimulateParticles", std::move(json)}}};
        return NormalizeVfxGpuVulkanReflection(capture.manifest, Target(), reflection, VfxGpuShaderReflectionNames(Role), compiled);
    }

    std::string Text(const Capture &capture) {
        return {capture.json.begin(), capture.json.end()};
    }

    std::string Replace(std::string value, std::string_view from, std::string_view to) {
        const auto position = value.find(from);
        REQUIRE(position != std::string::npos);
        value.replace(position, from.size(), to);
        return value;
    }
}  // namespace

// Genuine validated Vulkan1.3 SPIR-V/Cross JSON captured on the Metal producer's intermediate route.
// This exercises parser/proof reuse, not a claim that the new Vulkan producer or native device ran.
TEST_CASE("Canonical Vulkan VFX reflection consumes actual captured SPIR-V and JSON without MSL", "[renderer][vfx][vulkan][reflection]") {
    const Capture capture;
    const auto result = Normalize(capture, Text(capture));
    REQUIRE(result.HasValue());
    CHECK(result.Value().backend == ShaderTargetBackend::Vulkan);
    REQUIRE(result.Value().bindings.size() == 5);
    REQUIRE(result.Value().parameters.size() == 6);
    REQUIRE(result.Value().targetBindings.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(result.Value().targetBindings[i].nativeSpace == 0);
        CHECK(result.Value().targetBindings[i].nativeBinding == i);
    }
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(result.Value().parameters[i].byteOffset == i * 16);
}

TEST_CASE("Vulkan VFX rejects contradictory actual layout access workgroup and entry evidence", "[renderer][vfx][vulkan][reflection]") {
    const Capture capture;
    const auto original = Text(capture);
    const std::array changes{std::pair{"\"array_stride\" : 4", "\"array_stride\" : 8"},
                             std::pair{"\"readonly\" : true", "\"readonly\" : false"},
                             std::pair{"\"binding\" : 2", "\"binding\" : 0"},
                             std::pair{"\"set\" : 0", "\"set\" : 1"},
                             std::pair{"SimulateParticles", "ForeignParticles"},
                             std::pair{"64,", "32,"},
                             std::pair{"\"offset\" : 16", "\"offset\" : 20"},
                             std::pair{"\"type\" : \"uvec4\"", "\"type\" : \"vec4\""}};
    for (const auto &[from, to] : changes) {
        const auto result = Normalize(capture, Replace(original, from, to));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
    }
    CHECK(Normalize(capture, Replace(original, "{", "{\"textures\":[],")).HasError());
}

TEST_CASE("Vulkan VFX rejects mismatched packaged binary independently of valid JSON", "[renderer][vfx][vulkan][reflection]") {
    Capture capture;
    const auto original = Text(capture);
    // Corrupt an actual SPIR-V header, never author a positive native module.
    capture.spirv[0] ^= 1;
    CHECK(Normalize(capture, original).HasError());
}

TEST_CASE("Vulkan VFX requires exact ordinal-zero Cross names for genuine unnamed SPIR-V runtime members",
          "[renderer][vfx][vulkan][reflection]") {
    const Capture capture;
    const auto original = Text(capture);
    // The genuine module has no OpMemberName for either ByteAddressBuffer struct.
    // Cross actually emits _m0 for the single ordinal-zero runtime array proved by ProveSpirv.
    REQUIRE(Normalize(capture, original).HasValue());
    for (const auto name : {"_m1", "word", "value"})
        CHECK(Normalize(capture, Replace(original, "\"name\" : \"_m0\"", std::string{"\"name\" : \""} + name + "\"")).HasError());
}
