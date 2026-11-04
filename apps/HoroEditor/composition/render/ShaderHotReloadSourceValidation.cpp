#include "Horo/Runtime/Render/ShaderManifestErrors.h"
#include "ShaderHotReloadSourceInternal.h"

#include <algorithm>

namespace Horo::Application::Internal::ShaderSourceDetail {
    namespace {
        /** @brief Bounds semantic metadata before cloning caller-owned strings, paths or vectors. */
        struct MetadataBudget final {
            std::size_t remaining{1024U * 1024U};

            bool Charge(const std::size_t bytes) noexcept {
                if (bytes > remaining)
                    return false;
                remaining -= bytes;
                return true;
            }

            bool Text(const std::string &text) noexcept {
                return text.size() <= MaximumIdentityBytes && Charge(text.size());
            }

            bool Path(const std::filesystem::path &path) noexcept {
                return path.native().size() <= MaximumPathUnits && Charge(path.native().size() * sizeof(std::filesystem::path::value_type));
            }
        };

        bool EmptyCapturedPayload(const ShaderBuildRequest &build) noexcept {
            return build.compilation.source.empty() && build.compilation.dependencies.empty() && build.sources.empty() &&
                   !build.operationId;
        }

        bool SourceLimits(const Render::ShaderCompilerLimits &limits) noexcept {
            return limits.maximumSourceBytes > 0 && limits.maximumSourceBytes <= 16U * 1024U * 1024U && limits.maximumDependencyBytes > 0 &&
                   limits.maximumDependencyBytes <= 4U * 1024U * 1024U && limits.maximumDependencies <= 256 &&
                   limits.maximumTotalDependencyBytes <= 32U * 1024U * 1024U;
        }

        bool CompilerCounts(const ShaderBuildRequest &build) noexcept {
            return !build.compilation.targets.empty() && build.compilation.targets.size() <= 8 && build.compilation.defines.size() <= 128 &&
                   SourceLimits(build.limits);
        }

        bool TargetMetadata(const Render::ShaderCompilerTargetDescriptor &target, MetadataBudget &budget) {
            if (target.tools.size() > 16 || !budget.Text(target.platformTriple) ||
                !budget.Charge(target.tools.size() * sizeof(Render::ShaderCompilerToolIdentity)))
                return false;
            for (const auto &tool : target.tools)
                if (!budget.Text(tool.release))
                    return false;
            return true;
        }

        bool CompilerText(const ShaderBuildRequest &build, MetadataBudget &budget) {
            if (!budget.Charge(build.compilation.targets.size() * sizeof(Render::ShaderCompilerTargetDescriptor) +
                               build.compilation.defines.size() * sizeof(Render::ShaderCompilerDefine)))
                return false;
            for (const auto &define : build.compilation.defines)
                if (!budget.Text(define.name) || !budget.Text(define.value))
                    return false;
            for (const auto &target : build.compilation.targets)
                if (!TargetMetadata(target, budget))
                    return false;
            return true;
        }

        bool BindingReflectionMetadata(const Render::ShaderReflectionBindingName &binding, MetadataBudget &budget,
                                       std::size_t &remainingParameters) {
            if (!budget.Text(binding.name) || binding.parameters.size() > remainingParameters)
                return false;
            remainingParameters -= binding.parameters.size();
            if (!budget.Charge(binding.parameters.size() * sizeof(Render::ShaderReflectionParameterName)))
                return false;
            for (const auto &parameter : binding.parameters)
                if (!budget.Text(parameter.name))
                    return false;
            return true;
        }

        bool ReflectionMetadata(const ShaderBuildRequest &build, MetadataBudget &budget) {
            if (!build.compilation.reflection)
                return false;
            const auto &reflection = *build.compilation.reflection;
            if (reflection.names.size() > 64 || !budget.Text(reflection.tool.release) ||
                !budget.Charge(reflection.names.size() * sizeof(Render::ShaderReflectionBindingName)))
                return false;
            std::size_t remainingParameters = 512;
            for (const auto &binding : reflection.names)
                if (!BindingReflectionMetadata(binding, budget, remainingParameters))
                    return false;
            return true;
        }

        /** @brief Called only after the canonical validator admits every manifest vector/count/name. */
        bool ManifestMetadata(const Render::ShaderManifest &manifest, MetadataBudget &budget) {
            if (!budget.Text(manifest.sourceIdentity))
                return false;
            const std::size_t bytes = sizeof(manifest) + manifest.entryPoints.size() * sizeof(Render::ShaderEntryPoint) +
                                      manifest.bindings.size() * sizeof(Render::ShaderResourceBinding) +
                                      manifest.parameters.size() * sizeof(Render::ShaderParameter) +
                                      manifest.inlineConstants.size() * sizeof(Render::ShaderInlineConstantRange) +
                                      manifest.specializationInputs.size() * sizeof(Render::ShaderSpecializationInput) +
                                      manifest.targets.size() * sizeof(Render::ShaderTargetRequirement);
            if (!budget.Charge(bytes))
                return false;
            for (const auto &entry : manifest.entryPoints)
                if (!budget.Text(entry.name))
                    return false;
            return true;
        }

        bool UniqueDependencies(const std::vector<ShaderHotReloadSourceDependency> &dependencies) {
            for (std::size_t index = 0; index < dependencies.size(); ++index)
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (dependencies[index].logicalPath == dependencies[prior].logicalPath)
                        return false;
            return true;
        }

        bool SourceMetadata(const ShaderHotReloadSourceProgram &program, MetadataBudget &budget) {
            if (program.dependencies.size() > 256 || program.dependencies.size() > program.blueprint.limits.maximumDependencies ||
                !budget.Path(program.relativeSource) ||
                !budget.Charge(program.dependencies.size() * sizeof(ShaderHotReloadSourceDependency)))
                return false;
            if (!UniqueDependencies(program.dependencies))
                return false;
            for (const auto &dependency : program.dependencies)
                if (!budget.Path(dependency.relativePath) || !budget.Text(dependency.logicalPath))
                    return false;
            return true;
        }

        Result<void> ValidateProgram(const ShaderHotReloadSourceProgram &program, MetadataBudget &budget) {
            if (auto manifest = Render::ValidateShaderManifest(program.blueprint.compilation.manifest); manifest.HasError())
                return manifest;
            if (program.id.value == 0 || !EmptyCapturedPayload(program.blueprint))
                return Result<void>::Failure(SourceError("application.shader_reload.source_invalid",
                                                         "Use a valid program identity and metadata-only compiler blueprint."));
            if (!CompilerCounts(program.blueprint) || !CompilerText(program.blueprint, budget))
                return Result<void>::Failure(SourceError("application.shader_reload.source_capacity",
                                                         "Compiler metadata exceeds the bounded source-capture envelope."));
            if (!ReflectionMetadata(program.blueprint, budget) || !SourceMetadata(program, budget))
                return Result<void>::Failure(
                    SourceError("application.shader_reload.source_capacity",
                                "Source paths, declared dependencies or reflection names exceed the capture envelope."));
            if (!ManifestMetadata(program.blueprint.compilation.manifest, budget))
                return Result<void>::Failure(SourceError("application.shader_reload.source_capacity",
                                                         "Source manifest metadata exceeds the aggregate capture allowance."));
            return Result<void>::Success();
        }

        Result<std::size_t> FileSize(const std::filesystem::path &root, const std::filesystem::path &relative, const std::size_t limit) {
            auto path = SourcePath(root, relative);
            if (path.HasError())
                return Result<std::size_t>::Failure(path.ErrorValue());
            std::error_code error;
            const auto size = std::filesystem::file_size(path.Value(), error);
            if (error)
                return Result<std::size_t>::Failure(SourceError("application.shader_reload.source_read",
                                                                "Inspect the explicitly declared regular shader source before capture."));
            if (size > limit)
                return Result<std::size_t>::Failure(
                    SourceError("application.shader_reload.source_capacity", "Source file exceeds its admitted per-file allowance."));
            return Result<std::size_t>::Success(static_cast<std::size_t>(size));
        }

        Result<void> ChargeFile(const std::filesystem::path &root, const std::filesystem::path &path, const std::size_t limit,
                                std::size_t &remaining, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(
                    SourceError("application.shader_reload.source_cancelled", "Shader source capture was cancelled before reading."));
            auto size = FileSize(root, path, std::min(limit, remaining));
            if (size.HasError())
                return Result<void>::Failure(size.ErrorValue());
            remaining -= size.Value();
            return Result<void>::Success();
        }
    }  // namespace

    Error SourceError(const char *code, const char *message) {
        return Error{ErrorCode{code}, ErrorDomainId{"horo.application.shader_reload"}, ErrorSeverity::Error, message, {}};
    }

    Result<void> ValidateRecipe(const ShaderHotReloadSourceRecipe &recipe) {
        if (recipe.programs.empty() || recipe.programs.size() > MaximumPrograms || recipe.maximumCapturedBytes == 0 ||
            recipe.maximumCapturedBytes > MaximumCapturedBytes)
            return Result<void>::Failure(SourceError("application.shader_reload.source_capacity",
                                                     "Admit one to eight programs and a finite aggregate source allowance."));
        MetadataBudget budget;
        if (!budget.Path(recipe.root))
            return Result<void>::Failure(SourceError("application.shader_reload.source_capacity",
                                                     "The explicit source root exceeds its bounded native-path allowance."));
        for (std::size_t index = 0; index < recipe.programs.size(); ++index) {
            const auto &program = recipe.programs[index];
            if (index > 0 && recipe.programs[index - 1].id.value >= program.id.value)
                return Result<void>::Failure(
                    SourceError("application.shader_reload.source_invalid", "Provide unique program roles in canonical identity order."));
            if (auto validated = ValidateProgram(program, budget); validated.HasError())
                return validated;
        }
        return Result<void>::Success();
    }

    Result<std::filesystem::path> SourcePath(const std::filesystem::path &root, const std::filesystem::path &relative) {
        if (relative.empty() || relative.is_absolute())
            return Result<std::filesystem::path>::Failure(
                SourceError("application.shader_reload.source_invalid", "Declare nonempty source paths relative to the explicit root."));
        for (const auto &part : relative)
            if (part == "..")
                return Result<std::filesystem::path>::Failure(
                    SourceError("application.shader_reload.source_invalid", "Source paths cannot escape their declared root."));
        const auto path = (root / relative).lexically_normal();
        if (auto authority = Assets::AssetSourcePublicationLockPath(root, path); authority.HasError())
            return Result<std::filesystem::path>::Failure(authority.ErrorValue());
        return Result<std::filesystem::path>::Success(path);
    }

    Result<std::size_t> AdmitSourceSizes(const ShaderHotReloadSourceRecipe &recipe, const std::filesystem::path &root,
                                         const CancellationToken &cancellation) {
        std::size_t remaining = recipe.maximumCapturedBytes;
        for (const auto &program : recipe.programs) {
            if (auto admitted =
                    ChargeFile(root, program.relativeSource, program.blueprint.limits.maximumSourceBytes, remaining, cancellation);
                admitted.HasError())
                return Result<std::size_t>::Failure(admitted.ErrorValue());
            std::size_t dependencyRemaining = program.blueprint.limits.maximumTotalDependencyBytes;
            for (const auto &dependency : program.dependencies) {
                const auto before = remaining;
                if (auto admitted =
                        ChargeFile(root, dependency.relativePath,
                                   std::min(program.blueprint.limits.maximumDependencyBytes, dependencyRemaining), remaining, cancellation);
                    admitted.HasError())
                    return Result<std::size_t>::Failure(admitted.ErrorValue());
                dependencyRemaining -= before - remaining;
            }
        }
        return Result<std::size_t>::Success(recipe.maximumCapturedBytes - remaining);
    }
}  // namespace Horo::Application::Internal::ShaderSourceDetail

namespace Horo::Application::Internal {
    /** @copydoc ValidateShaderHotReloadSourceRecipe */
    Result<void> ValidateShaderHotReloadSourceRecipe(const ShaderHotReloadSourceRecipe &recipe) {
        return ShaderSourceDetail::ValidateRecipe(recipe);
    }
}  // namespace Horo::Application::Internal
