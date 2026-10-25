#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
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
        const auto &identity = request.tool;
        if (!cross || cross->identity.release != identity.release || cross->identity.buildDigest != identity.buildDigest)
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
