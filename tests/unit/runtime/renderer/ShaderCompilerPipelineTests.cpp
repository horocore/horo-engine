#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/ShaderManifestErrors.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] Sha256Digest Digest(const std::uint8_t marker) {
        Sha256Digest result;
        result.bytes.front() = marker;
        return result;
    }

    [[nodiscard]] ShaderTargetRequirement Requirement(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        return {.backend = backend,
                .payloadFormat = format,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    [[nodiscard]] ShaderCompilerToolIdentity Tool(const ShaderCompilerTool tool, const std::uint8_t marker) {
        return {tool, "test-1.0+locked", Digest(marker)};
    }

    [[nodiscard]] ShaderCompilerTargetDescriptor Target(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        ShaderCompilerTargetDescriptor target;
        target.requirement = Requirement(backend, format);
        switch (backend) {
            case ShaderTargetBackend::Null:
                target.platformTriple = "headless-null";
                break;
            case ShaderTargetBackend::OpenGL:
                target.platformTriple = "desktop-opengl";
                target.intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16;
                target.tools = {Tool(ShaderCompilerTool::Dxc, 1), Tool(ShaderCompilerTool::SpirvTools, 2),
                                Tool(ShaderCompilerTool::SpirvCross, 3)};
                break;
            case ShaderTargetBackend::Vulkan:
                target.platformTriple = "desktop-vulkan";
                target.intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16;
                target.tools = {Tool(ShaderCompilerTool::Dxc, 1), Tool(ShaderCompilerTool::SpirvTools, 2)};
                break;
            case ShaderTargetBackend::Metal:
                target.platformTriple = "macos14-arm64-metal";
                target.intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16;
                target.tools = {Tool(ShaderCompilerTool::Dxc, 1), Tool(ShaderCompilerTool::SpirvTools, 2),
                                Tool(ShaderCompilerTool::SpirvCross, 3), Tool(ShaderCompilerTool::AppleMetal, 4)};
                break;
            case ShaderTargetBackend::D3D12:
                target.platformTriple = "windows-x64-d3d12";
                target.tools = {Tool(ShaderCompilerTool::Dxc, 1), Tool(ShaderCompilerTool::DxilValidator, 5)};
                break;
        }
        return target;
    }

    [[nodiscard]] ShaderCompilationRequest ValidRequest() {
        ShaderCompilationRequest request;
        request.manifest = {.schemaVersion = 1,
                            .sourceIdentity = "shaders.standard.surface",
                            .sourceRevision = 17,
                            .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                            .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                                          ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment}},
                            .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 1, 4, 1}},
                            .inlineConstants = {},
                            .specializationInputs = {},
                            .targets = {Requirement(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture),
                                        Requirement(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410),
                                        Requirement(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16),
                                        Requirement(ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24),
                                        Requirement(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60)}};
        request.source = {1, 2, 3, 4, 5};
        request.dependencies = {{"shaders/common.hlsli", Digest(20)}, {"shaders/lighting.hlsli", Digest(21)}};
        request.defines = {{"ALPHA_MASK", "0"}, {"NORMAL_MAP", "1"}};
        request.targets = {Target(ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture),
                           Target(ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410),
                           Target(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16),
                           Target(ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24),
                           Target(ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60)};
        return request;
    }

    class RecordingAdapter final : public IShaderCompilerAdapter {
    public:
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                  const CancellationToken &) const override {
            keys.push_back(invocation.artifactKey);
            backends.push_back(invocation.target.requirement.backend);
            ShaderCompilerAdapterOutput output;
            output.backend = invocation.target.requirement.backend;
            output.payloadFormat = invocation.target.requirement.payloadFormat;
            output.payload = {static_cast<std::uint8_t>(output.backend), 0x7fU};
            if (invocation.target.emitDebugInformation)
                output.debugPayload = {0xddU};
            output.diagnostics = {{ShaderCompilerDiagnosticCategory::Toolchain,
                                   ShaderCompilerDiagnosticSeverity::Information,
                                   {},
                                   0,
                                   0,
                                   "compiled by locked test toolchain",
                                   false}};
            return Result<ShaderCompilerAdapterOutput>::Success(std::move(output));
        }

        mutable std::vector<Sha256Digest> keys;
        mutable std::vector<ShaderTargetBackend> backends;
    };

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Shader compiler pipeline produces one deterministic artifact per exact target", "[runtime][renderer][shader-compiler]") {
    const ShaderCompilationRequest request = ValidRequest();
    RecordingAdapter firstAdapter;
    const auto first = CompileShaderTargets(request, firstAdapter, {});
    REQUIRE(first.HasValue());
    CHECK(first.Value().artifacts.size() == request.targets.size());
    CHECK(first.Value().dependencies.size() == request.dependencies.size());
    CHECK(firstAdapter.backends == std::vector{ShaderTargetBackend::Null, ShaderTargetBackend::OpenGL, ShaderTargetBackend::Vulkan,
                                               ShaderTargetBackend::Metal, ShaderTargetBackend::D3D12});
    CHECK(std::ranges::all_of(first.Value().artifacts, [](const CompiledShaderArtifact &artifact) {
        return !artifact.payload.empty() && artifact.diagnostics.size() == 1;
    }));

    RecordingAdapter secondAdapter;
    const auto second = CompileShaderTargets(request, secondAdapter, {});
    REQUIRE(second.HasValue());
    CHECK(firstAdapter.keys == secondAdapter.keys);
    CHECK(std::ranges::adjacent_find(firstAdapter.keys) == firstAdapter.keys.end());
}

TEST_CASE("Shader artifact identity covers source dependencies options and pinned tools", "[runtime][renderer][shader-compiler]") {
    const ShaderCompilationRequest baseline = ValidRequest();
    RecordingAdapter baselineAdapter;
    REQUIRE(CompileShaderTargets(baseline, baselineAdapter, {}).HasValue());

    const auto changedKey = [&](auto mutate) {
        ShaderCompilationRequest changed = baseline;
        mutate(changed);
        RecordingAdapter adapter;
        REQUIRE(CompileShaderTargets(changed, adapter, {}).HasValue());
        return adapter.keys.front();
    };
    CHECK(changedKey([](ShaderCompilationRequest &request) {
        request.source.push_back(9);
    }) != baselineAdapter.keys.front());
    CHECK(changedKey([](ShaderCompilationRequest &request) {
        request.dependencies.front().digest = Digest(44);
    }) != baselineAdapter.keys.front());
    CHECK(changedKey([](ShaderCompilationRequest &request) {
        request.defines.front().value = "1";
    }) != baselineAdapter.keys.front());
    CHECK(changedKey([](ShaderCompilationRequest &request) {
        request.targets.front().optimization = ShaderOptimizationLevel::Size;
    }) != baselineAdapter.keys.front());

    ShaderCompilationRequest changedTool = baseline;
    changedTool.targets[1].tools.front().buildDigest = Digest(77);
    RecordingAdapter changedToolAdapter;
    REQUIRE(CompileShaderTargets(changedTool, changedToolAdapter, {}).HasValue());
    CHECK(changedToolAdapter.keys.front() == baselineAdapter.keys.front());
    CHECK(changedToolAdapter.keys[1] != baselineAdapter.keys[1]);
}

TEST_CASE("Shader compiler pipeline rejects malformed bounds and noncanonical immutable inputs", "[runtime][renderer][shader-compiler]") {
    RecordingAdapter adapter;
    ShaderCompilerLimits limits;
    limits.maximumTargets = 0;
    RequireError(CompileShaderTargets(ValidRequest(), adapter, {}, limits), ShaderCompilerPipelineErrors::InvalidLimits);

    ShaderCompilationRequest request = ValidRequest();
    request.source.clear();
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::InvalidRequest);

    request = ValidRequest();
    std::swap(request.dependencies[0], request.dependencies[1]);
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::NonCanonicalInput);

    request = ValidRequest();
    std::swap(request.defines[0], request.defines[1]);
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::NonCanonicalInput);

    request = ValidRequest();
    request.targets.front().requirement.maximumBindings = 15;
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::InvalidRequest);

    request = ValidRequest();
    request.dependencies.front().logicalPath = "../outside.hlsli";
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::InvalidRequest);
}

TEST_CASE("Shader target routes require exact baselines and pinned build identities", "[runtime][renderer][shader-compiler]") {
    RecordingAdapter adapter;
    ShaderCompilationRequest request = ValidRequest();
    request.targets[1].intermediateEnvironment = ShaderIntermediateEnvironment::None;
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::UnsupportedTarget);

    request = ValidRequest();
    request.targets[2].tools.front().buildDigest = {};
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::UnpinnedToolchain);

    request = ValidRequest();
    std::swap(request.targets[3].tools[1], request.targets[3].tools[2]);
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::UnpinnedToolchain);

    request = ValidRequest();
    request.targets[4].disableAutomaticDepthRemap = false;
    RequireError(CompileShaderTargets(request, adapter, {}), ShaderCompilerPipelineErrors::UnsupportedTarget);
}

TEST_CASE("Shader compilation cancellation is checked before and between adapter calls", "[runtime][renderer][shader-compiler]") {
    RecordingAdapter adapter;
    CancellationSource alreadyCancelled;
    alreadyCancelled.RequestCancellation();
    RequireError(CompileShaderTargets(ValidRequest(), adapter, alreadyCancelled.Token()),
                 ShaderCompilerPipelineErrors::CancellationRequested);
    CHECK(adapter.backends.empty());

    class CancellingAdapter final : public IShaderCompilerAdapter {
    public:
        explicit CancellingAdapter(CancellationSource source) : source_(std::move(source)) {}

        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                  const CancellationToken &) const override {
            source_.RequestCancellation();
            return Result<ShaderCompilerAdapterOutput>::Success(
                {invocation.target.requirement.backend, invocation.target.requirement.payloadFormat, {1}, {}, {}});
        }

    private:
        CancellationSource source_;
    };

    CancellationSource duringCompile;
    CancellingAdapter cancelling(duringCompile);
    RequireError(CompileShaderTargets(ValidRequest(), cancelling, duringCompile.Token()),
                 ShaderCompilerPipelineErrors::CancellationRequested);
}

TEST_CASE("Shader compiler pipeline preserves adapter failures and rejects invalid candidate output",
          "[runtime][renderer][shader-compiler]") {
    class FailingAdapter final : public IShaderCompilerAdapter {
    public:
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &,
                                                                  const CancellationToken &) const override {
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderManifestErrors::InvalidManifest));
        }
    } failing;

    const auto failure = CompileShaderTargets(ValidRequest(), failing, {});
    RequireError(failure, ShaderCompilerPipelineErrors::AdapterFailure);
    CHECK(
        ErrorChainContains(failure.ErrorValue(), ShaderManifestErrors::InvalidManifest.domain, ShaderManifestErrors::InvalidManifest.code));

    class MismatchedAdapter final : public IShaderCompilerAdapter {
    public:
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &,
                                                                  const CancellationToken &) const override {
            return Result<ShaderCompilerAdapterOutput>::Success({ShaderTargetBackend::D3D12, ShaderPayloadFormat::Dxil60, {1}, {}, {}});
        }
    } mismatched;

    RequireError(CompileShaderTargets(ValidRequest(), mismatched, {}), ShaderCompilerPipelineErrors::InvalidAdapterOutput);

    class ThrowingAdapter final : public IShaderCompilerAdapter {
    public:
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &,
                                                                  const CancellationToken &) const override {
            throw std::runtime_error("private tool failure");
        }
    } throwing;

    RequireError(CompileShaderTargets(ValidRequest(), throwing, {}), ShaderCompilerPipelineErrors::AdapterFailure);
}

TEST_CASE("Shader compiler pipeline enforces payload diagnostic and debug bounds", "[runtime][renderer][shader-compiler]") {
    class InvalidOutputAdapter final : public IShaderCompilerAdapter {
    public:
        enum class Mode {
            Payload,
            Diagnostic,
            Debug
        } mode{Mode::Payload};

        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                  const CancellationToken &) const override {
            ShaderCompilerAdapterOutput output{invocation.target.requirement.backend,
                                               invocation.target.requirement.payloadFormat,
                                               {1},
                                               {},
                                               {}};
            if (mode == Mode::Payload)
                output.payload.resize(invocation.limits.maximumPayloadBytes + 1U);
            if (mode == Mode::Diagnostic)
                output.diagnostics = {{ShaderCompilerDiagnosticCategory::Source, ShaderCompilerDiagnosticSeverity::Error, "shader.hlsl", 0,
                                       1, "bad location", false}};
            if (mode == Mode::Debug)
                output.debugPayload = {1};
            return Result<ShaderCompilerAdapterOutput>::Success(std::move(output));
        }
    } adapter;

    ShaderCompilerLimits limits;
    limits.maximumPayloadBytes = 4;
    RequireError(CompileShaderTargets(ValidRequest(), adapter, {}, limits), ShaderCompilerPipelineErrors::InvalidAdapterOutput);
    adapter.mode = InvalidOutputAdapter::Mode::Diagnostic;
    RequireError(CompileShaderTargets(ValidRequest(), adapter, {}), ShaderCompilerPipelineErrors::InvalidAdapterOutput);
    adapter.mode = InvalidOutputAdapter::Mode::Debug;
    RequireError(CompileShaderTargets(ValidRequest(), adapter, {}), ShaderCompilerPipelineErrors::InvalidAdapterOutput);

    class LocalPathDiagnosticAdapter final : public IShaderCompilerAdapter {
    public:
        [[nodiscard]] Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                                  const CancellationToken &) const override {
            return Result<ShaderCompilerAdapterOutput>::Success(
                {invocation.target.requirement.backend,
                 invocation.target.requirement.payloadFormat,
                 {1},
                 {},
                 {{ShaderCompilerDiagnosticCategory::Source, ShaderCompilerDiagnosticSeverity::Error, "/tmp/shader.hlsl", 1, 1,
                   "raw host path", false}}});
        }
    } localPath;

    RequireError(CompileShaderTargets(ValidRequest(), localPath, {}), ShaderCompilerPipelineErrors::InvalidAdapterOutput);
}
