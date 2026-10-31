#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/ShaderManifestErrors.h"
#include "ShaderCompilerPipelineTestFixture.h"
#include "support/TypedIdentityTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Tests::RequireError;
    using namespace Horo::Render::ShaderCompilerPipelineTests;

    class CountingDiagnosticSink final : public IShaderCompilerDiagnosticSink {
    public:
        std::size_t published{};

        Result<void> BeginPhase(const ShaderCompilerDiagnosticContext &) override {
            return Result<void>::Success();
        }

        Result<void> Publish(const ShaderCompilerDiagnostic &) override {
            ++published;
            return Result<void>::Success();
        }
    };

    class RejectingStreamAdapter final : public IShaderCompilerAdapter {
    public:
        enum class Invalid {
            Location,
            ToolCode,
            Context,
            Flood
        } invalid{Invalid::Location};

        Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation, const CancellationToken &) const override {
            ShaderCompilerDiagnostic diagnostic;
            diagnostic.message = "diagnostic";
            if (invalid == Invalid::Location)
                diagnostic.sourceIdentity = "/tmp/private.hlsl";
            if (invalid == Invalid::ToolCode)
                diagnostic.toolCode = std::string(invocation.limits.maximumIdentityBytes + 1, 'x');
            if (invalid == Invalid::Context) {
                diagnostic.context = ShaderCompilerDiagnosticContext{ShaderCompilerPhase::SourceCompilation,
                                                                     invocation.target.requirement.backend,
                                                                     invocation.manifest.sourceRevision + 1,
                                                                     {},
                                                                     {},
                                                                     {}};
            }
            const std::size_t count = invalid == Invalid::Flood ? invocation.limits.maximumDiagnosticsPerTarget + 1 : 1;
            for (std::size_t index = 0; index < count; ++index)
                static_cast<void>(invocation.diagnostics->Publish(diagnostic));
            return Result<ShaderCompilerAdapterOutput>::Success(
                {invocation.target.requirement.backend, invocation.target.requirement.payloadFormat, {1}, {}, {}});
        }
    };

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
        request.dependencies.front() = Dependency("shaders/common.hlsli", {44});
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

    request = ValidRequest();
    request.dependencies.front().content.push_back(99);
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

TEST_CASE("Pipeline validates streamed diagnostics before host publication even when an adapter ignores rejection",
          "[shader][build_output]") {
    CountingDiagnosticSink sink;
    RejectingStreamAdapter adapter;

    for (const auto invalid :
         {RejectingStreamAdapter::Invalid::Location, RejectingStreamAdapter::Invalid::ToolCode, RejectingStreamAdapter::Invalid::Context}) {
        adapter.invalid = invalid;
        RequireError(CompileShaderTargets(ValidRequest(), adapter, {}, {}, &sink), ShaderCompilerPipelineErrors::InvalidAdapterOutput);
        CHECK(sink.published == 0);
    }
    adapter.invalid = RejectingStreamAdapter::Invalid::Flood;
    ShaderCompilerLimits limits;
    limits.maximumDiagnosticsPerTarget = 2;
    RequireError(CompileShaderTargets(ValidRequest(), adapter, {}, limits, &sink), ShaderCompilerPipelineErrors::InvalidAdapterOutput);
    CHECK(sink.published == 2);
}

TEST_CASE("Legacy compiler diagnostics are forwarded once and host sink failure is preserved", "[shader][build_output]") {
    class Sink final : public IShaderCompilerDiagnosticSink {
    public:
        std::size_t count{};
        bool reject{};

        Result<void> BeginPhase(const ShaderCompilerDiagnosticContext &) override {
            return Result<void>::Success();
        }

        Result<void> Publish(const ShaderCompilerDiagnostic &) override {
            ++count;
            return reject ? Result<void>::Failure(MakeError(ShaderManifestErrors::InvalidManifest)) : Result<void>::Success();
        }
    } sink;

    RecordingAdapter adapter;
    const auto request = ValidRequest();
    REQUIRE(CompileShaderTargets(request, adapter, {}, {}, &sink).HasValue());
    CHECK(sink.count == request.targets.size());
    sink.count = 0;
    sink.reject = true;
    RequireError(CompileShaderTargets(request, adapter, {}, {}, &sink), ShaderManifestErrors::InvalidManifest);
    CHECK(sink.count == 1);
}

TEST_CASE("Compiler result retains exactly the requested immutable same-cook reflection", "[runtime][renderer][shader-compiler]") {
    auto request = ValidRequest();
    request.targets = {request.targets[2]};
    request.manifest.targets = {request.manifest.targets[2]};
    request.reflection = ShaderCompilerReflectionRequest{Tool(ShaderCompilerTool::SpirvCross, 3), {{{1}, "Parameters", {{{1}, "Tint"}}}}};
    ReflectedAdapter adapter;
    const auto result = CompileShaderTargets(request, adapter, {});
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().artifacts.size() == 1);
    const auto copy = result.Value();
    const auto &artifact = copy.artifacts.front();
    REQUIRE(artifact.reflection != nullptr);
    CHECK(artifact.reflection == result.Value().artifacts.front().reflection);
    CHECK(artifact.reflection->compiledArtifactKey == artifact.artifactKey);
    CHECK(artifact.reflection->payloadDigest == ComputeSha256(std::as_bytes(std::span{artifact.payload})));
    auto renamed = request;
    renamed.reflection->names.front().parameters.front().name = "OtherTint";
    const auto changed = CompileShaderTargets(renamed, adapter, {});
    REQUIRE(changed.HasValue());
    CHECK(changed.Value().artifacts.front().artifactKey != artifact.artifactKey);
}

TEST_CASE("Compiler result rejects missing foreign or stale reflected records", "[runtime][renderer][shader-compiler]") {
    auto request = ValidRequest();
    request.targets = {request.targets[2]};
    request.manifest.targets = {request.manifest.targets[2]};
    request.reflection = ShaderCompilerReflectionRequest{Tool(ShaderCompilerTool::SpirvCross, 3), {{{1}, "Parameters", {{{1}, "Tint"}}}}};
    ReflectedAdapter adapter;
    SECTION("Missing") {
        adapter.mutation = ReflectedAdapter::Mutation::Missing;
    }
    SECTION("Key") {
        adapter.mutation = ReflectedAdapter::Mutation::Key;
    }
    SECTION("Payload") {
        adapter.mutation = ReflectedAdapter::Mutation::Payload;
    }
    SECTION("Tool") {
        adapter.mutation = ReflectedAdapter::Mutation::Tool;
    }
    SECTION("Interface") {
        adapter.mutation = ReflectedAdapter::Mutation::Interface;
    }
    SECTION("Valid alternate mapping with stale cooked identity") {
        adapter.mutation = ReflectedAdapter::Mutation::Mapping;
    }
    CHECK(CompileShaderTargets(request, adapter, {}).HasError());
}

TEST_CASE("Compiler bounds and validates logical reflection requests before invoking tools", "[runtime][renderer][shader-compiler]") {
    auto request = ValidRequest();
    request.reflection = ShaderCompilerReflectionRequest{Tool(ShaderCompilerTool::SpirvCross, 3), {{{1}, "Parameters", {{{1}, "Tint"}}}}};
    SECTION("Duplicate binding") {
        request.reflection->names.push_back(request.reflection->names.front());
    }
    SECTION("Duplicate parameter") {
        request.reflection->names.front().parameters.push_back({{1}, "Other"});
    }
    SECTION("Foreign logical member") {
        request.reflection->names.front().parameters.front().id = {2};
    }
    SECTION("Oversized authored name") {
        request.reflection->names.front().name.resize(513, 'x');
    }
    SECTION("Wrong tool role") {
        request.reflection->tool.tool = ShaderCompilerTool::Dxc;
    }
    RecordingAdapter adapter;
    CHECK(CompileShaderTargets(request, adapter, {}).HasError());
    CHECK(adapter.keys.empty());
}
