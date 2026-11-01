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
        Result<Render::ShaderManifest> Manifest(Editor::EditorViewportShaderProgram role, const Render::ShaderTargetRequirement &target) {
            if (target.backend != Render::ShaderTargetBackend::Vulkan)
                return Editor::MakeEditorViewportShaderManifest(role, target);
            return Result<Render::ShaderManifest>::Success(role == Editor::EditorViewportShaderProgram::Shadow
                                                               ? Editor::EditorVulkanShadowShaderManifest()
                                                               : Editor::EditorVulkanViewportShaderManifest());
        }

        std::vector<Render::ShaderReflectionBindingName> Names(Editor::EditorViewportShaderProgram role,
                                                               Render::ShaderTargetBackend backend) {
            if (backend != Render::ShaderTargetBackend::Vulkan)
                return Editor::EditorViewportShaderReflectionNames(role);
            return role == Editor::EditorViewportShaderProgram::Shadow ? Editor::EditorVulkanShadowReflectionNames()
                                                                       : Editor::EditorVulkanViewportReflectionNames();
        }

        Result<ShaderHotReloadSourceProgram> MakeProgram(const Editor::EditorViewportShaderProgram role,
                                                         const Render::ShaderCompilerTargetDescriptor &target,
                                                         const Render::ShaderCompilerToolIdentity &reflectionTool) {
            const bool vulkan = target.requirement.backend == Render::ShaderTargetBackend::Vulkan;
            auto manifest = Manifest(role, target.requirement);
            if (manifest.HasError())
                return Result<ShaderHotReloadSourceProgram>::Failure(manifest.ErrorValue());
            ShaderHotReloadSourceProgram program;
            program.id = {static_cast<std::uint32_t>(role)};
            program.relativeSource = vulkan && role == Editor::EditorViewportShaderProgram::Lit
                                         ? std::filesystem::path{"shaders/editor/viewport.hlsl"}
                                         : std::filesystem::path{Editor::EditorViewportShaderSource(role)};
            program.blueprint.compilation.manifest = std::move(manifest).Value();
            program.blueprint.compilation.targets = {target};
            program.blueprint.limits.maximumSourceBytes = 1024U * 1024U;
            program.blueprint.limits.maximumPayloadBytes = 8U * 1024U * 1024U;
            program.blueprint.limits.maximumDebugPayloadBytes = 8U * 1024U * 1024U;
            program.blueprint.limits.maximumDiagnosticsPerTarget = 64;
            program.blueprint.limits.maximumDiagnosticMessageBytes = 2048;
            program.blueprint.compilation.reflection =
                Render::ShaderCompilerReflectionRequest{reflectionTool, Names(role, target.requirement.backend)};
            return Result<ShaderHotReloadSourceProgram>::Success(std::move(program));
        }

        bool LegacyMetalProgramEnvelope(const ShaderHotReloadSourceProgram &program) {
            const auto &request = program.blueprint.compilation;
            return program.id.value >= 1 && program.id.value <= 3 && request.targets.size() == 1 && request.reflection &&
                   request.targets.front().requirement.backend == Render::ShaderTargetBackend::Metal;
        }

        bool CanonicalLegacyMetalProgram(const ShaderHotReloadSourceProgram &program) {
            if (!LegacyMetalProgramEnvelope(program))
                return false;
            const auto role = static_cast<Editor::EditorViewportShaderProgram>(program.id.value);
            if (program.relativeSource != Editor::EditorViewportShaderSource(role))
                return false;
            const auto &request = program.blueprint.compilation;
            const auto expected = Editor::MakeEditorViewportShaderManifest(role, request.targets.front().requirement);
            if (expected.HasError())
                return false;
            const auto expectedInterface = Render::ComputeShaderInterfaceCompatibilityId(expected.Value());
            const auto actualInterface = Render::ComputeShaderInterfaceCompatibilityId(request.manifest);
            return expectedInterface.HasValue() && actualInterface.HasValue() && expectedInterface.Value() == actualInterface.Value();
        }

        Result<void> MatchClusteredProgram(ShaderHotReloadSourceProgram &program) {
            const auto role = static_cast<Editor::EditorViewportShaderProgram>(program.id.value);
            const auto manifest =
                Editor::MakeEditorViewportClusteredShaderManifest(role, program.blueprint.compilation.targets.front().requirement);
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            program.relativeSource = Editor::EditorViewportClusteredShaderSource(role);
            program.blueprint.compilation.manifest = manifest.Value();
            program.blueprint.compilation.reflection->names = Editor::EditorViewportClusteredShaderReflectionNames(role);
            return Result<void>::Success();
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
        for (std::uint32_t role = 1; role <= 3; ++role) {
            if (target.requirement.backend == Render::ShaderTargetBackend::Vulkan &&
                role == static_cast<std::uint32_t>(Editor::EditorViewportShaderProgram::Grid))
                continue;
            auto program = MakeProgram(static_cast<Editor::EditorViewportShaderProgram>(role), target, reflectionTool);
            if (program.HasError())
                return Output::Failure(program.ErrorValue());
            configuration.sources.programs.push_back(std::move(program).Value());
        }
        if (auto valid = ValidateShaderHotReloadSourceRecipe(configuration.sources); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        return Output::Success(std::move(configuration));
    }

    Result<ShaderHotReloadHostConfiguration> MatchEditorViewportHotReloadFamily(const ShaderHotReloadHostConfiguration &configuration,
                                                                                const bool nativeClustered) {
        using Outcome = Result<ShaderHotReloadHostConfiguration>;
        if (const auto valid = ValidateShaderHotReloadSourceRecipe(configuration.sources); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (!nativeClustered || configuration.sources.programs.size() != 3)
            return Outcome::Success(configuration);
        if (!std::ranges::all_of(configuration.sources.programs, CanonicalLegacyMetalProgram))
            return Outcome::Success(configuration);
        auto matched = configuration;
        for (auto &program : matched.sources.programs) {
            const auto updated = MatchClusteredProgram(program);
            if (updated.HasError())
                return Outcome::Failure(updated.ErrorValue());
        }
        if (const auto valid = ValidateShaderHotReloadSourceRecipe(matched.sources); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        return Outcome::Success(std::move(matched));
    }
}  // namespace Horo::Application::Internal
