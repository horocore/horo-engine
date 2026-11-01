#include "EditorViewportShaderRecipe.h"
#include "EditorVulkanViewportShader.h"
#include "ShaderHotReloadHost.h"

#include <algorithm>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Requires explicit nonzero editor-session scope before the target supplies its device identity. */
        bool AdmittedScope(const ShaderHotReloadFence &fence) noexcept {
            return fence.projectGeneration && fence.configurationRevision && fence.lifecycleGeneration && fence.sourceGeneration;
        }

        /** @brief Bounds route metadata and requires the requested reflector in its qualified tool closure. */
        bool AdmittedTarget(const Render::ShaderCompilerTargetDescriptor &target,
                            const Render::ShaderCompilerToolIdentity &reflectionTool) {
            if (target.platformTriple.size() > 256 || target.tools.size() > 16 || reflectionTool.release.size() > 256)
                return false;
            for (const auto &tool : target.tools)
                if (tool.release.size() > 256)
                    return false;
            return std::ranges::any_of(target.tools, [&reflectionTool](const auto &tool) {
                return tool.tool == reflectionTool.tool && tool.release == reflectionTool.release &&
                       tool.buildDigest == reflectionTool.buildDigest;
            });
        }

        /** @brief Builds one metadata-only canonical role; no reflected offsets or native slots are invented. */
        Result<ShaderHotReloadSourceProgram> MakeProgram(const Editor::EditorViewportShaderProgram role,
                                                         const Render::ShaderCompilerTargetDescriptor &target,
                                                         const Render::ShaderCompilerToolIdentity &reflectionTool) {
            const bool vulkan = target.requirement.backend == Render::ShaderTargetBackend::Vulkan;
            auto manifest = vulkan ? Result<Render::ShaderManifest>::Success(Editor::EditorVulkanViewportShaderManifest())
                                   : Editor::MakeEditorViewportShaderManifest(role, target.requirement);
            if (manifest.HasError())
                return Result<ShaderHotReloadSourceProgram>::Failure(manifest.ErrorValue());
            ShaderHotReloadSourceProgram program;
            program.id = {static_cast<std::uint32_t>(role)};
            program.relativeSource = vulkan ? std::filesystem::path{"shaders/editor/viewport.hlsl"}
                                            : std::filesystem::path{Editor::EditorViewportShaderSource(role)};
            program.blueprint.compilation.manifest = std::move(manifest).Value();
            program.blueprint.compilation.targets = {target};
            program.blueprint.limits.maximumSourceBytes = 1024U * 1024U;
            program.blueprint.limits.maximumPayloadBytes = 8U * 1024U * 1024U;
            program.blueprint.limits.maximumDebugPayloadBytes = 8U * 1024U * 1024U;
            program.blueprint.limits.maximumDiagnosticsPerTarget = 64;
            program.blueprint.limits.maximumDiagnosticMessageBytes = 2048;
            program.blueprint.compilation.reflection =
                Render::ShaderCompilerReflectionRequest{reflectionTool, vulkan ? Editor::EditorVulkanViewportReflectionNames()
                                                                               : Editor::EditorViewportShaderReflectionNames(role)};
            return Result<ShaderHotReloadSourceProgram>::Success(std::move(program));
        }
    }  // namespace

    /** @copydoc MakeEditorViewportHotReloadConfiguration */
    Result<ShaderHotReloadHostConfiguration> MakeEditorViewportHotReloadConfiguration(
        const std::filesystem::path &root, const Render::ShaderCompilerTargetDescriptor &target,
        const Render::ShaderCompilerToolIdentity &reflectionTool, const ShaderHotReloadFence fence) {
        using Output = Result<ShaderHotReloadHostConfiguration>;
        if (!AdmittedScope(fence) || !root.is_absolute() || root.native().size() > 4096 || !AdmittedTarget(target, reflectionTool))
            return Output::Failure(
                MakeError(ShaderHotReloadErrors::InvalidRequest,
                          "Use explicit bounded host identities and a qualified reflection tool from the selected route."));
        ShaderHotReloadHostConfiguration configuration;
        configuration.publication = {.policy = ShaderHotReloadPolicy::Development, .context = fence};
        configuration.sources.root = root;
        const auto last = target.requirement.backend == Render::ShaderTargetBackend::Vulkan ? 1U : 3U;
        for (std::uint32_t role = 1; role <= last; ++role) {
            auto program = MakeProgram(static_cast<Editor::EditorViewportShaderProgram>(role), target, reflectionTool);
            if (program.HasError())
                return Output::Failure(program.ErrorValue());
            configuration.sources.programs.push_back(std::move(program).Value());
        }
        if (auto valid = ValidateShaderHotReloadSourceRecipe(configuration.sources); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        return Output::Success(std::move(configuration));
    }
}  // namespace Horo::Application::Internal
