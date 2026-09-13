#include "Horo/Runtime/Render/ShaderManifest.h"
#include "Horo/Runtime/Render/ShaderManifestErrors.h"
#include "support/ShaderTestSupport.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace ShaderManifestTests {
    using namespace Horo;
    using namespace Horo::Render;
    using Tests::RequireError;

    [[nodiscard]] ShaderTargetRequirement Target(const ShaderTargetBackend backend, const ShaderPayloadFormat payload) {
        return {.backend = backend,
                .payloadFormat = payload,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    [[nodiscard]] ShaderManifest ValidManifest() {
        return {.schemaVersion = 1,
                .sourceIdentity = "shaders.standard.surface",
                .sourceRevision = 7,
                .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                .bindings = Tests::StandardSurfaceBindings(),
                .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 1, 4, 1}},
                .inlineConstants = {{0, 16, ShaderStageVisibility::Vertex}},
                .specializationInputs = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 0, ShaderStageVisibility::Fragment}},
                .targets = {Target(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture),
                            Target(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410)}};
    }

}  // namespace ShaderManifestTests

using Horo::FormatSha256;
using Horo::Render::ComputeShaderInterfaceCompatibilityId;
using Horo::Render::ShaderBindingId;
using Horo::Render::ShaderManifest;
using Horo::Render::ShaderManifestLimits;
using Horo::Render::ShaderParameterId;
using Horo::Render::ShaderPayloadFormat;
using Horo::Render::ShaderResourceAccess;
using Horo::Render::ShaderResourceKind;
using Horo::Render::ShaderSpecializationId;
using Horo::Render::ShaderStage;
using Horo::Render::ShaderStageVisibility;
using Horo::Render::ShaderTargetBackend;
using Horo::Render::ShaderValueType;
using Horo::Render::ValidateShaderManifest;
using Horo::Tests::RequireError;
using ShaderManifestTests::Target;
using ShaderManifestTests::ValidManifest;

namespace ShaderManifestErrors = Horo::Render::ShaderManifestErrors;

TEST_CASE("Shader manifest accepts a canonical backend-neutral interface", "[runtime][renderer][shader-manifest]") {
    const ShaderManifest manifest = ValidManifest();
    CHECK(ValidateShaderManifest(manifest).HasValue());
    const auto identity = ComputeShaderInterfaceCompatibilityId(manifest);
    REQUIRE(identity.HasValue());
    CHECK(FormatSha256(identity.Value().digest) == "sha256:6bfe7681557c5b6f32bd5d102bba52e570fbfc252c20b3e8964920e8881516dc");
}

TEST_CASE("Shader compatibility identity excludes source revision and target-native placement", "[runtime][renderer][shader-manifest]") {
    ShaderManifest first = ValidManifest();
    ShaderManifest second = first;
    second.sourceIdentity = "shaders.generated.surface";
    second.sourceRevision = 99;
    second.targets = {Target(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16),
                      Target(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60)};

    const auto firstIdentity = ComputeShaderInterfaceCompatibilityId(first);
    const auto secondIdentity = ComputeShaderInterfaceCompatibilityId(second);
    REQUIRE(firstIdentity.HasValue());
    REQUIRE(secondIdentity.HasValue());
    CHECK(firstIdentity.Value() == secondIdentity.Value());

    second.parameters.front().columns = 3;
    const auto changedInterface = ComputeShaderInterfaceCompatibilityId(second);
    REQUIRE(changedInterface.HasValue());
    CHECK(changedInterface.Value() != firstIdentity.Value());
}

TEST_CASE("Shader manifest rejects malformed versions identities and noncanonical records", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.schemaVersion = 2;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);

    manifest = ValidManifest();
    manifest.sourceIdentity = "shader with spaces";
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);

    manifest = ValidManifest();
    manifest.entryPoints.push_back({ShaderStage::Vertex, "VertexMain"});
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);

    manifest = ValidManifest();
    manifest.entryPoints.front().name = "1VertexMain";
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);

    manifest = ValidManifest();
    manifest.entryPoints.front().name = "Vertex.Main";
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);

    manifest = ValidManifest();
    std::swap(manifest.bindings[0], manifest.bindings[1]);
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);

    ShaderManifestLimits limits;
    limits.maximumTargets = 0;
    RequireError(ValidateShaderManifest(ValidManifest(), limits), ShaderManifestErrors::InvalidLimits);
}

TEST_CASE("Shader manifest supports canonical multiple entry points per stage", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.entryPoints.insert(manifest.entryPoints.begin() + 1, {ShaderStage::Vertex, "VertexShadow"});
    CHECK(ValidateShaderManifest(manifest).HasValue());

    std::swap(manifest.entryPoints[0], manifest.entryPoints[1]);
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);
}

TEST_CASE("Shader binding and parameter validation rejects invalid access and references", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.bindings[1].access = ShaderResourceAccess::ReadWrite;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);

    manifest = ValidManifest();
    manifest.bindings[1].stages = ShaderStageVisibility::Compute;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidReference);

    manifest = ValidManifest();
    manifest.parameters.front().binding = ShaderBindingId{2};
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidReference);

    manifest = ValidManifest();
    manifest.specializationInputs.front().defaultValueBits = 2;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
}

TEST_CASE("Shader inline constants use aligned bounded nonoverlapping byte ranges", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.inlineConstants.push_back({16, 16, ShaderStageVisibility::Fragment});
    CHECK(ValidateShaderManifest(manifest).HasValue());

    manifest.inlineConstants.back().byteOffset = 12;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidInlineConstants);

    manifest = ValidManifest();
    manifest.inlineConstants.front().byteOffset = std::numeric_limits<std::uint32_t>::max() - 3U;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidInlineConstants);

    manifest = ValidManifest();
    manifest.targets.front().maximumInlineConstantBytes = 8;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);
}

TEST_CASE("Shader targets fail closed on route feature and capacity mismatches", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.targets.front().payloadFormat = ShaderPayloadFormat::SpirV16;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);

    manifest = ValidManifest();
    manifest.targets.front().maximumBindings = 2;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);

    manifest = ValidManifest();
    manifest.entryPoints = {{ShaderStage::Compute, "ComputeMain"}};
    manifest.bindings = {
        {ShaderBindingId{1}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadWrite, 1, ShaderStageVisibility::Compute}};
    manifest.parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Uint32, 1, 1, 1}};
    manifest.inlineConstants.clear();
    manifest.specializationInputs.clear();
    manifest.targets = {Target(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16)};
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);

    manifest.targets.front().supportsCompute = true;
    manifest.targets.front().supportsStorageResources = true;
    CHECK(ValidateShaderManifest(manifest).HasValue());
}

TEST_CASE("Shader manifest enforces each finite collection boundary", "[runtime][renderer][shader-manifest]") {
    const ShaderManifest manifest = ValidManifest();
    ShaderManifestLimits limits;
    limits.maximumEntryPoints = 1;
    RequireError(ValidateShaderManifest(manifest, limits), ShaderManifestErrors::InvalidManifest);
    limits = {};
    limits.maximumBindings = 2;
    RequireError(ValidateShaderManifest(manifest, limits), ShaderManifestErrors::InvalidManifest);
    limits = {};
    limits.maximumParameters = 1;
    ShaderManifest twoParameters = manifest;
    twoParameters.parameters.push_back({ShaderParameterId{2}, ShaderBindingId{1}, ShaderValueType::Uint32, 1, 1, 1});
    RequireError(ValidateShaderManifest(twoParameters, limits), ShaderManifestErrors::InvalidManifest);
    limits = {};
    limits.maximumSpecializationInputs = 1;
    ShaderManifest twoSpecializations = manifest;
    twoSpecializations.specializationInputs.push_back(
        {ShaderSpecializationId{2}, ShaderValueType::Uint32, 1, ShaderStageVisibility::Vertex});
    RequireError(ValidateShaderManifest(twoSpecializations, limits), ShaderManifestErrors::InvalidManifest);
}

TEST_CASE("Shader manifest validates every logical record boundary", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.bindings.front().arrayCount = 0;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
    manifest = ValidManifest();
    manifest.bindings.front().kind = static_cast<ShaderResourceKind>(0xFFU);
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
    manifest = ValidManifest();
    manifest.parameters.front().rows = 5;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
    manifest = ValidManifest();
    manifest.parameters.front().arrayCount = 0;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
    manifest = ValidManifest();
    manifest.inlineConstants.front().byteSize = 6;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidInlineConstants);
    manifest = ValidManifest();
    manifest.inlineConstants.front().stages = ShaderStageVisibility::Compute;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidInlineConstants);
    manifest = ValidManifest();
    manifest.specializationInputs.front().id = {};
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);
    manifest = ValidManifest();
    manifest.specializationInputs.front().stages = ShaderStageVisibility::Compute;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidReference);
}

TEST_CASE("Shader target descriptors require canonical unique routes and exact schema versions", "[runtime][renderer][shader-manifest]") {
    ShaderManifest manifest = ValidManifest();
    manifest.targets.clear();
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::InvalidManifest);
    manifest = ValidManifest();
    manifest.targets.front().descriptorVersion = 2;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);
    manifest = ValidManifest();
    manifest.targets.front().interfaceSchemaVersion = 2;
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::UnsupportedTarget);
    manifest = ValidManifest();
    manifest.targets.push_back(Target(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410));
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);
    manifest = ValidManifest();
    std::swap(manifest.targets[0], manifest.targets[1]);
    RequireError(ValidateShaderManifest(manifest), ShaderManifestErrors::NonCanonicalIdentity);
}
