#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "ShaderCompilerRoute.h"
#include "ShaderCompilerToolchainSupport.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace Horo::Render::ShaderCompilerToolchainDetail {
    namespace {
        /** @brief Runs only the reviewed exact executable with the same shell-free scratch/time/output/cancellation contract. */
        Result<void> RunReflection(const ShaderCompilerToolchainConfiguration &configuration, IExternalProcessRunner &processes,
                                   const ShaderCompilerToolInstallation &tool, const std::filesystem::path &input,
                                   const std::filesystem::path &output, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            ExternalProcessRequest request;
            request.executable = tool.executable.string();
            request.arguments = {input.string(), "--reflect", "--output", output.string()};
            request.workingDirectory = input.parent_path();
            request.environment.base = ProcessEnvironmentBase::Replace;
            request.timeout = configuration.processTimeout;
            request.maximumLineBytes = std::min(std::size_t{8192}, configuration.maximumProcessOutputBytes);
            request.maximumOutputBytes = configuration.maximumProcessOutputBytes;
            const auto completed = processes.Run(request, cancellation);
            if (completed.HasError())
                return Result<void>::Failure(WrapError(ShaderCompilerPipelineErrors::ToolProcessFailed, completed.ErrorValue()));
            if (cancellation.IsCancellationRequested() || completed.Value().reason == ProcessTerminationReason::Cancelled)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            if (completed.Value().reason == ProcessTerminationReason::TimedOut)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolTimedOut));
            if (completed.Value().reason != ProcessTerminationReason::Exited || completed.Value().exitCode != 0)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolProcessFailed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc AdmitRasterCookInterface */
    Result<bool> AdmitRasterCookInterface(const ShaderCompilerInvocation &invocation) {
        const auto canonicalVfx = MakeVfxShaderManifest(invocation.target.requirement);
        const auto expectedVfx = ComputeShaderInterfaceCompatibilityId(canonicalVfx);
        const auto actual = ComputeShaderInterfaceCompatibilityId(invocation.manifest);
        if (expectedVfx.HasError() || actual.HasError())
            return Result<bool>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        const bool vfxInterface = expectedVfx.Value() == actual.Value();
        if (vfxInterface && invocation.manifest.sourceRevision != canonicalVfx.sourceRevision)
            return Result<bool>::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
        if (vfxInterface && invocation.target.requirement.backend == ShaderTargetBackend::OpenGL &&
            invocation.target.intermediateEnvironment != ShaderIntermediateEnvironment::Vulkan12SpirV15)
            return Result<bool>::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
        return Result<bool>::Success(vfxInterface);
    }

    /** @copydoc RasterTranslationArguments */
    std::vector<std::string> RasterTranslationArguments(const ShaderEntryPoint &entry, const std::filesystem::path &spirvPath,
                                                        const std::filesystem::path &nativePath, const bool metal,
                                                        const bool vfxInterface) {
        std::vector<std::string> arguments{spirvPath.string(),
                                           "--output",
                                           nativePath.string(),
                                           "--entry",
                                           entry.name,
                                           "--stage",
                                           std::string{StageName(entry.stage)}};
        if (metal)
            arguments.insert(arguments.end(), {"--msl", "--msl-version", "20400"});
        else
            arguments.insert(arguments.end(), {"--version", "410", "--no-es", "--no-420pack-extension"});
        if (vfxInterface)
            arguments.emplace_back(metal ? "--msl-decoration-binding" : "--no-support-nonzero-baseinstance");
        if (vfxInterface && !metal) {
            // Monolithic GLSL410 links stage varyings by name. The native location alone does not
            // reconcile DXC's distinct in_var/out_var prefixes on the supported Mac GL profile.
            constexpr std::array names{"horo_vfx_uv", "horo_vfx_color"};
            for (std::size_t location = 0; location < names.size(); ++location)
                arguments.insert(arguments.end(), {"--rename-interface-variable", entry.stage == ShaderStage::Vertex ? "out" : "in",
                                                   std::to_string(location), names[location]});
        }
        return arguments;
    }

    /** @copydoc NormalizeCapturedRasterReflection */
    Result<NormalizedShaderReflection> NormalizeCapturedRasterReflection(const ShaderCompilerInvocation &invocation,
                                                                         const std::vector<SpirvReflectionStage> &stages,
                                                                         const std::vector<std::string> &translations) {
        const auto &target = invocation.target.requirement;
        if (target.backend == ShaderTargetBackend::OpenGL)
            return NormalizeVfxOpenGLReflection(invocation.manifest, target, stages, translations);
        const auto expected = ComputeShaderInterfaceCompatibilityId(MakeVfxShaderManifest(target));
        const auto actual = ComputeShaderInterfaceCompatibilityId(invocation.manifest);
        if (expected.HasError() || actual.HasError())
            return Result<NormalizedShaderReflection>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        if (expected.Value() == actual.Value())
            return NormalizeVfxMetalReflection(invocation.manifest, target, stages, translations);
        return NormalizeUiPaintMetalReflection(invocation.manifest, target, stages, translations);
    }

    /** @copydoc ValidateUiPaintMetalReflection */
    Result<void> ValidateUiPaintMetalReflection(const ShaderCompilerReflectionRequest &request,
                                                const ShaderCompilerToolInstallation *cross) {
        constexpr std::array<std::string_view, 4> names{"PaintParameters", "PaintTexture", "CoverageTexture", "PaintSampler"};
        if (request.names.size() != names.size())
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        for (std::size_t binding = 0; binding < names.size(); ++binding) {
            const auto &declared = request.names[binding];
            if (declared.id.value != binding + 1 || declared.name != names[binding] || !declared.parameters.empty())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        }
        if (const auto &identity = request.tool;
            !cross || cross->identity.release != identity.release || cross->identity.buildDigest != identity.buildDigest)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
        return Result<void>::Success();
    }

    /** @copydoc ValidateRasterReflection */
    Result<void> ValidateRasterReflection(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                          const ShaderCompilerReflectionRequest &request, const ShaderCompilerToolInstallation *cross) {
        const auto expected = ComputeShaderInterfaceCompatibilityId(MakeVfxShaderManifest(target));
        const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
        if (expected.HasError() || actual.HasError())
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        if (target.backend != ShaderTargetBackend::Metal && target.backend != ShaderTargetBackend::OpenGL)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        if (expected.Value() != actual.Value()) {
            if (target.backend == ShaderTargetBackend::OpenGL)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            return ValidateUiPaintMetalReflection(request, cross);
        }
        const std::array<ShaderReflectionBindingName, 4>
            names{ShaderReflectionBindingName{{1}, "VfxInstances", {{{1}, "ClipFromLocal"}, {{2}, "InstanceColor"}}},
                  ShaderReflectionBindingName{{2}, "VfxMaterial", {{{3}, "Tint"}, {{4}, "AlphaCutoff"}}},
                  ShaderReflectionBindingName{{3}, "BaseColor", {}}, ShaderReflectionBindingName{{4}, "BaseColorSampler", {}}};
        if (request.names.size() != names.size())
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto &given = request.names[index], &wanted = names[index];
            if (given.id != wanted.id || given.name != wanted.name || given.parameters.size() != wanted.parameters.size())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            for (std::size_t member = 0; member < wanted.parameters.size(); ++member)
                if (given.parameters[member].id != wanted.parameters[member].id ||
                    given.parameters[member].name != wanted.parameters[member].name)
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        }
        if (!cross || request.tool.tool != ShaderCompilerTool::SpirvCross || cross->identity.release != request.tool.release ||
            cross->identity.buildDigest != request.tool.buildDigest)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
        return Result<void>::Success();
    }

    /** @copydoc ReflectVulkanRoute */
    Result<std::vector<SpirvReflectionStage>> ReflectVulkanRoute(const ShaderCompilerToolchainConfiguration &configuration,
                                                                 IExternalProcessRunner &processes, const CompiledShaderArtifact &compiled,
                                                                 const ShaderCompilerToolIdentity &tool,
                                                                 const CancellationToken &cancellation,
                                                                 const std::filesystem::path &scratch) {
        using Outcome = Result<std::vector<SpirvReflectionStage>>;
        const auto installed = std::ranges::find_if(configuration.tools, [&](const auto &candidate) {
            return tool.tool == ShaderCompilerTool::SpirvCross && candidate.identity.tool == tool.tool &&
                   candidate.identity.release == tool.release && candidate.identity.buildDigest == tool.buildDigest;
        });
        if (installed == configuration.tools.end())
            return Outcome::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
        const auto decoded =
            DecodeShaderArtifactStages(compiled.payload, ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 64U * 1024U * 1024U);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        std::vector<SpirvReflectionStage> result;
        result.reserve(decoded.Value().stageCount);
        for (std::size_t index = 0; index < decoded.Value().stageCount; ++index) {
            const auto &stage = decoded.Value().stages[index];
            const auto input = scratch / std::format("stage-{}.spv", index);
            const auto output = scratch / std::format("stage-{}.reflection.json", index);
            if (const auto written = WriteFile(input, stage.bytes); written.HasError())
                return Outcome::Failure(written.ErrorValue());
            if (const auto ran = RunReflection(configuration, processes, *installed, input, output, cancellation); ran.HasError())
                return Outcome::Failure(ran.ErrorValue());
            const auto json = ReadBoundedFile(output, 1024U * 1024U, ShaderCompilerPipelineErrors::ToolOutputInvalid);
            if (json.HasError())
                return Outcome::Failure(json.ErrorValue());
            result.push_back({stage.stage, std::string{stage.entryPoint}, std::string{json.Value().begin(), json.Value().end()}});
        }
        return Outcome::Success(std::move(result));
    }
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
