/** @file
 * @brief Target-private request builders and scripted adapters for compiler identity and reflection admission tests.
 */
#pragma once

#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <utility>

namespace Horo::Render::ShaderCompilerPipelineTests {
    [[nodiscard]] inline Sha256Digest Digest(const std::uint8_t marker) {
        Sha256Digest result;
        result.bytes.front() = marker;
        return result;
    }

    [[nodiscard]] inline ShaderTargetRequirement Requirement(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
        return {.backend = backend,
                .payloadFormat = format,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    [[nodiscard]] inline ShaderCompilerToolIdentity Tool(const ShaderCompilerTool tool, const std::uint8_t marker) {
        return {tool, "test-1.0+locked", Digest(marker)};
    }

    [[nodiscard]] inline ShaderCompilerDependency Dependency(std::string logicalPath, std::vector<std::uint8_t> content) {
        const Sha256Digest digest = ComputeSha256(std::as_bytes(std::span{content}));
        return {std::move(logicalPath), digest, std::move(content)};
    }

    [[nodiscard]] inline ShaderCompilerTargetDescriptor Target(const ShaderTargetBackend backend, const ShaderPayloadFormat format) {
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

    [[nodiscard]] inline ShaderCompilationRequest ValidRequest() {
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
        request.dependencies = {Dependency("shaders/common.hlsli", {20}), Dependency("shaders/lighting.hlsli", {21})};
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

    /** @brief Scripted metadata adapter exercises result ownership/admission only, never actual tool qualification. */
    class ReflectedAdapter final : public IShaderCompilerAdapter {
    public:
        enum class Mutation {
            None,
            Missing,
            Key,
            Payload,
            Tool,
            Interface,
            Mapping
        } mutation{Mutation::None};

        Result<ShaderCompilerAdapterOutput> Compile(const ShaderCompilerInvocation &invocation,
                                                    const CancellationToken &token) const override {
            RecordingAdapter compiler;
            auto result = compiler.Compile(invocation, token);
            if (result.HasError() || mutation == Mutation::Missing)
                return result;
            auto output = std::move(result).Value();
            REQUIRE(invocation.reflection != nullptr);
            ShaderReflectionCandidate candidate{.backend = invocation.target.requirement.backend,
                                                .interfaceSchemaVersion = invocation.target.requirement.interfaceSchemaVersion};
            candidate.bindings.push_back({{1},
                                          ShaderResourceKind::UniformBuffer,
                                          ShaderResourceAccess::ReadOnly,
                                          1,
                                          ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment,
                                          true});
            candidate.parameters.push_back({{1}, {1}, ShaderValueType::Float32, 1, 4, 1});
            candidate.targetBindings.push_back({.id = {1}});
            auto normalized = NormalizeShaderReflection(invocation.manifest, invocation.target.requirement, candidate);
            REQUIRE(normalized.HasValue());
            const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VertexMain", {1}},
                                    ShaderArtifactStage{ShaderStage::Fragment, "FragmentMain", {2}}};
            auto payload = PackageShaderArtifactStages(invocation.target.requirement.backend, invocation.target.requirement.payloadFormat,
                                                       stages, invocation.limits.maximumPayloadBytes);
            REQUIRE(payload.HasValue());
            output.payload = std::move(payload).Value();
            CompiledShaderArtifact compiled{output.backend,
                                            output.payloadFormat,
                                            invocation.artifactKey,
                                            std::move(output.payload),
                                            {},
                                            {}};
            auto cooked = CookReflectedShaderArtifact(compiled, invocation.reflection->tool, std::move(normalized).Value());
            output.payload = std::move(compiled.payload);
            REQUIRE(cooked.HasValue());
            auto record = std::make_shared<ReflectedShaderArtifact>(std::move(cooked).Value());
            ApplyMutation(*record, invocation);
            output.reflection = std::move(record);
            return Result<ShaderCompilerAdapterOutput>::Success(std::move(output));
        }

    private:
        /** @brief Mutates one same-cook record while keeping each deliberately invalid identity independent. */
        void ApplyMutation(ReflectedShaderArtifact &record, const ShaderCompilerInvocation &invocation) const {
            if (mutation == Mutation::Key)
                record.compiledArtifactKey = Digest(8);
            if (mutation == Mutation::Payload)
                record.payloadDigest = Digest(8);
            if (mutation == Mutation::Tool)
                record.reflectionTool.release += "other";
            if (mutation == Mutation::Interface)
                record.reflection.interfaceCompatibility = {};
            if (mutation == Mutation::Mapping) {
                record.reflection.targetBindings.front().nativeBinding = 5;
                const auto &value = record.reflection;
                const ShaderReflectionCandidate alternate{value.backend,    value.interfaceSchemaVersion, value.bindings,
                                                          value.parameters, value.stageInterface,         value.targetBindings,
                                                          value.sourceMap};
                REQUIRE(NormalizeShaderReflection(invocation.manifest, invocation.target.requirement, alternate).HasValue());
            }
        }
    };

}  // namespace Horo::Render::ShaderCompilerPipelineTests
