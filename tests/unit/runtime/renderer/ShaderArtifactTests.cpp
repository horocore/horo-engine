#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace ShaderArtifactTests {
    using namespace Horo::Render;

    // Existing cooker version-one bytes: one OpenGL vertex entry named main, payload x.
    constexpr std::array<std::uint8_t, 36> ExistingPackage{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R', 1,   0, 0, 0, 1, 1, 1, 0, 0, 0,
                                                           0,   4,   0,   0,   0,   'm', 'a', 'i', 'n', 1, 0, 0, 0, 0, 0, 0, 0, 'x'};

    [[nodiscard]] auto Decode(const std::span<const std::uint8_t> bytes) {
        return DecodeShaderArtifactStages(bytes, ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, 4'096);
    }
}  // namespace ShaderArtifactTests

using namespace ShaderArtifactTests;

TEST_CASE("Runtime shader decoder preserves the existing cooked package wire format", "[render][shader][artifact]") {
    const auto decoded = Decode(ExistingPackage);
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().stageCount == 1);
    CHECK(decoded.Value().stages[0].stage == ShaderStage::Vertex);
    CHECK(decoded.Value().stages[0].entryPoint == "main");
    CHECK(decoded.Value().stages[0].bytes.data() == ExistingPackage.data() + 35);
    CHECK(decoded.Value().stages[0].bytes.size() == 1);
    const std::array records{ShaderArtifactStage{ShaderStage::Vertex, "main", {'x'}}};
    const auto packaged = PackageShaderArtifactStages(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, records, 36);
    REQUIRE(packaged.HasValue());
    CHECK(std::ranges::equal(packaged.Value(), ExistingPackage));
    CHECK(PackageShaderArtifactStages(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, records, 35).HasError());
}

TEST_CASE("Every truncated shader package and appended byte fails closed", "[render][shader][artifact]") {
    for (std::size_t size = 0; size < ExistingPackage.size(); ++size) {
        INFO(size);
        Horo::Tests::RequireError(Decode(std::span{ExistingPackage}.first(size)), ShaderCompilerPipelineErrors::ToolOutputInvalid);
    }
    std::vector<std::uint8_t> trailing(ExistingPackage.begin(), ExistingPackage.end());
    trailing.push_back(0);
    CHECK(Decode(trailing).HasError());
}

TEST_CASE("Shader package validates exact family encoding version and bounded lengths", "[render][shader][artifact]") {
    for (const std::size_t index : {0U, 8U, 12U, 13U, 14U, 18U, 19U, 27U}) {
        auto corrupt = ExistingPackage;
        corrupt[index] = 255;
        INFO(index);
        CHECK(Decode(corrupt).HasError());
    }
    CHECK(DecodeShaderArtifactStages(ExistingPackage, ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 4'096).HasError());
    CHECK(DecodeShaderArtifactStages(ExistingPackage, ShaderTargetBackend::OpenGL, ShaderPayloadFormat::SpirV16, 4'096).HasError());
    CHECK(DecodeShaderArtifactStages(ExistingPackage, ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410,
                                     std::numeric_limits<std::size_t>::max())
              .HasError());
    auto empty = ExistingPackage;
    empty[27] = 0;
    CHECK(Decode(std::span{empty}.first(35)).HasError());
}

TEST_CASE("Stage packages reject duplicate entries and malformed identifiers before allocation", "[render][shader][artifact]") {
    std::array records{ShaderArtifactStage{ShaderStage::Vertex, "main", {'x'}}, ShaderArtifactStage{ShaderStage::Vertex, "main", {'y'}}};
    CHECK(PackageShaderArtifactStages(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, records, 4'096).HasError());
    records[1].stage = ShaderStage::Fragment;
    const auto valid = PackageShaderArtifactStages(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, records, 4'096);
    REQUIRE(valid.HasValue());
    REQUIRE(Decode(valid.Value()).HasValue());
    auto duplicate = valid.Value();
    duplicate[36] = 0;
    CHECK(Decode(duplicate).HasError());
    for (const std::string name : {"1main", "main\n", "bad.name", ""}) {
        records[1].entryPoint = name;
        CHECK(PackageShaderArtifactStages(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, records, 4'096).HasError());
    }
}
