#include "Horo/Runtime/Render/PipelineCache.h"
#include "Horo/Runtime/Render/PipelineCacheErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Tests::RequireError;

    [[nodiscard]] Sha256Digest Digest(const std::string_view text) {
        return ComputeSha256(std::as_bytes(std::span{text.data(), text.size()}));
    }

    [[nodiscard]] PipelineCacheCompatibility Compatibility(const std::string &driverVersion = "551.86") {
        return {.schemaVersion = 1,
                .backend = RenderBackendId{"vulkan"},
                .backendModuleVersion = 7,
                .adapter = RenderAdapterId{"pci-10de-2684"},
                .deviceCompatibilityDigest = Digest("device-cache-uuid"),
                .driverIdentity = "nvidia-proprietary",
                .driverVersion = driverVersion,
                .shaderBackend = ShaderTargetBackend::Vulkan,
                .shaderFormat = ShaderPayloadFormat::SpirV16,
                .shaderArtifactKey = Digest("shader-artifact"),
                .shaderInterface = {Digest("shader-interface")},
                .pipelineDescriptorDigest = Digest("pipeline-descriptor")};
    }

}  // namespace

TEST_CASE("Pipeline cache identity includes backend device driver shader and descriptor compatibility",
          "[runtime][renderer][pipeline-cache]") {
    auto baseline = ComputePipelineCacheKey(Compatibility());
    REQUIRE(baseline.HasValue());

    auto changedDriver = ComputePipelineCacheKey(Compatibility("552.12"));
    REQUIRE(changedDriver.HasValue());
    CHECK(changedDriver.Value() != baseline.Value());

    auto changedModule = Compatibility();
    ++changedModule.backendModuleVersion;
    auto moduleKey = ComputePipelineCacheKey(changedModule);
    REQUIRE(moduleKey.HasValue());
    CHECK(moduleKey.Value() != baseline.Value());

    auto changedPipeline = Compatibility();
    changedPipeline.pipelineDescriptorDigest = Digest("different-pipeline");
    auto pipelineKey = ComputePipelineCacheKey(changedPipeline);
    REQUIRE(pipelineKey.HasValue());
    CHECK(pipelineKey.Value() != baseline.Value());
}

TEST_CASE("Pipeline cache blob round trips only under its exact compatibility key", "[runtime][renderer][pipeline-cache]") {
    const auto key = ComputePipelineCacheKey(Compatibility());
    REQUIRE(key.HasValue());
    const std::vector<std::uint8_t> payload{1, 3, 3, 7};
    const auto serialized = SerializePipelineCacheBlob(key.Value(), payload);
    REQUIRE(serialized.HasValue());

    const auto loaded = LoadPipelineCacheBlob(key.Value(), serialized.Value());
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value() == payload);

    const auto otherKey = ComputePipelineCacheKey(Compatibility("different-driver"));
    REQUIRE(otherKey.HasValue());
    RequireError(LoadPipelineCacheBlob(otherKey.Value(), serialized.Value()), PipelineCacheErrors::IncompatibleBlob);
}

TEST_CASE("Pipeline cache blob rejects version corruption integrity failure and size overflow", "[runtime][renderer][pipeline-cache]") {
    const auto key = ComputePipelineCacheKey(Compatibility());
    REQUIRE(key.HasValue());
    const std::vector<std::uint8_t> payload{2, 4, 6, 8};
    const auto serialized = SerializePipelineCacheBlob(key.Value(), payload);
    REQUIRE(serialized.HasValue());

    auto wrongVersion = serialized.Value();
    wrongVersion[8] = 2;
    RequireError(LoadPipelineCacheBlob(key.Value(), wrongVersion), PipelineCacheErrors::UnsupportedVersion);

    auto corrupt = serialized.Value();
    corrupt.back() ^= 0xffU;
    RequireError(LoadPipelineCacheBlob(key.Value(), corrupt), PipelineCacheErrors::CorruptBlob);

    PipelineCacheLimits tight;
    tight.maximumPayloadBytes = 3;
    RequireError(SerializePipelineCacheBlob(key.Value(), payload, tight), PipelineCacheErrors::PayloadTooLarge);
    RequireError(LoadPipelineCacheBlob(key.Value(), serialized.Value(), tight), PipelineCacheErrors::PayloadTooLarge);
}

TEST_CASE("Pipeline cache rejects incomplete compatibility and invalid limits", "[runtime][renderer][pipeline-cache]") {
    auto incomplete = Compatibility();
    incomplete.driverIdentity.clear();
    RequireError(ComputePipelineCacheKey(incomplete), PipelineCacheErrors::InvalidCompatibility);

    PipelineCacheLimits invalid;
    invalid.maximumIdentityBytes = 0;
    RequireError(ComputePipelineCacheKey(Compatibility(), invalid), PipelineCacheErrors::InvalidLimits);
}

TEST_CASE("Pipeline cache admits printable driver identities and rejects control bytes", "[runtime][renderer][pipeline-cache]") {
    auto printable = Compatibility("NVIDIA 551.86");
    printable.driverIdentity = "NVIDIA Proprietary";
    CHECK(ComputePipelineCacheKey(printable).HasValue());

    auto controlByte = printable;
    controlByte.driverVersion = "Mesa\t23.2";
    RequireError(ComputePipelineCacheKey(controlByte), PipelineCacheErrors::InvalidCompatibility);
}
