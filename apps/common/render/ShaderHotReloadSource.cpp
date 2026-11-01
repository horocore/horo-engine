#include "ShaderHotReloadSourceInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using namespace ShaderSourceDetail;

        struct CapturedFile final {
            std::filesystem::path path;
            Assets::AssetSourceCapture source;
        };

        /** @brief The canonical reader repeats physical containment, per-file bounds and change checks after size preflight. */
        Result<CapturedFile> ReadSource(const std::filesystem::path &root, const std::filesystem::path &relative, const std::size_t maximum,
                                        std::size_t &remaining, const CancellationToken &cancellation) {
            if (remaining == 0)
                return Result<CapturedFile>::Failure(
                    SourceError("application.shader_reload.source_capacity", "No aggregate allowance remains for another source capture."));
            auto path = SourcePath(root, relative);
            if (path.HasError())
                return Result<CapturedFile>::Failure(path.ErrorValue());
            auto captured = Assets::CaptureAssetSource(root, path.Value(), std::min(maximum, remaining), cancellation);
            if (captured.HasError())
                return Result<CapturedFile>::Failure(captured.ErrorValue());
            if (!captured.Value().identity.exists)
                return Result<CapturedFile>::Failure(
                    SourceError("application.shader_reload.source_read", "Every explicitly declared shader source must exist."));
            remaining -= captured.Value().bytes.size();
            return Result<CapturedFile>::Success({std::move(path).Value(), std::move(captured).Value()});
        }

        /** @brief Byte representation conversion retains at most one additional bounded file, never an unbounded stream. */
        std::vector<std::uint8_t> CompilerBytes(const Assets::AssetSourceCapture &captured) {
            std::vector<std::uint8_t> bytes(captured.bytes.size());
            std::ranges::transform(captured.bytes, bytes.begin(), [](const std::byte value) {
                return std::to_integer<std::uint8_t>(value);
            });
            return bytes;
        }

        Result<void> CaptureMain(const ShaderHotReloadSourceProgram &program, const std::filesystem::path &root, ShaderBuildRequest &build,
                                 std::size_t &remaining, const CancellationToken &cancellation) {
            auto read = ReadSource(root, program.relativeSource, program.blueprint.limits.maximumSourceBytes, remaining, cancellation);
            if (read.HasError())
                return Result<void>::Failure(read.ErrorValue());
            auto file = std::move(read).Value();
            if (file.source.bytes.empty())
                return Result<void>::Failure(
                    SourceError("application.shader_reload.source_read", "A canonical shader source cannot be empty."));
            build.compilation.source = CompilerBytes(file.source);
            build.sources.push_back({build.compilation.manifest.sourceIdentity, std::move(file.path)});
            return Result<void>::Success();
        }

        Result<void> CaptureDependencies(const ShaderHotReloadSourceProgram &program, const std::filesystem::path &root,
                                         ShaderBuildRequest &build, std::size_t &remaining, const CancellationToken &cancellation) {
            std::size_t dependencyRemaining = build.limits.maximumTotalDependencyBytes;
            for (const auto &dependency : program.dependencies) {
                auto read = ReadSource(root, dependency.relativePath, std::min(build.limits.maximumDependencyBytes, dependencyRemaining),
                                       remaining, cancellation);
                if (read.HasError())
                    return Result<void>::Failure(read.ErrorValue());
                auto file = std::move(read).Value();
                dependencyRemaining -= file.source.bytes.size();
                build.compilation.dependencies.push_back({dependency.logicalPath, file.source.identity.digest, CompilerBytes(file.source)});
                build.sources.push_back({dependency.logicalPath, std::move(file.path)});
            }
            return Result<void>::Success();
        }

        Result<ShaderHotReloadProgram> CaptureProgram(const ShaderHotReloadSourceProgram &program, const std::filesystem::path &root,
                                                      std::size_t &remaining, const CancellationToken &cancellation) {
            ShaderHotReloadProgram result{program.id, program.blueprint};
            result.build.sources.reserve(program.dependencies.size() + 1);
            result.build.compilation.dependencies.reserve(program.dependencies.size());
            if (auto main = CaptureMain(program, root, result.build, remaining, cancellation); main.HasError())
                return Result<ShaderHotReloadProgram>::Failure(main.ErrorValue());
            if (auto dependencies = CaptureDependencies(program, root, result.build, remaining, cancellation); dependencies.HasError())
                return Result<ShaderHotReloadProgram>::Failure(dependencies.ErrorValue());
            return Result<ShaderHotReloadProgram>::Success(std::move(result));
        }

        Result<ShaderHotReloadRequest> CaptureFamily(const ShaderHotReloadSourceRecipe &recipe, const ShaderHotReloadFence &fence,
                                                     const CancellationToken &cancellation) {
            if (!recipe.root.is_absolute())
                return Result<ShaderHotReloadRequest>::Failure(
                    SourceError("application.shader_reload.source_read", "Choose an explicit absolute source root."));
            std::error_code error;
            const auto root = std::filesystem::canonical(recipe.root, error);
            if (error)
                return Result<ShaderHotReloadRequest>::Failure(
                    SourceError("application.shader_reload.source_read", "Choose an existing explicit absolute source root."));
            if (auto sizes = AdmitSourceSizes(recipe, root, cancellation); sizes.HasError())
                return Result<ShaderHotReloadRequest>::Failure(sizes.ErrorValue());
            ShaderHotReloadRequest request;
            request.fence = fence;
            request.programs.reserve(recipe.programs.size());
            std::size_t remaining = recipe.maximumCapturedBytes;
            for (const auto &program : recipe.programs) {
                auto captured = CaptureProgram(program, root, remaining, cancellation);
                if (captured.HasError())
                    return Result<ShaderHotReloadRequest>::Failure(captured.ErrorValue());
                request.programs.push_back(std::move(captured).Value());
            }
            return Result<ShaderHotReloadRequest>::Success(std::move(request));
        }
    }  // namespace

    /** @copydoc CaptureShaderHotReloadSources */
    Result<ShaderHotReloadRequest> CaptureShaderHotReloadSources(const ShaderHotReloadSourceRecipe &recipe,
                                                                 const ShaderHotReloadFence &fence, const CancellationToken &cancellation) {
        try {
            if (!fence.IsValid())
                return Result<ShaderHotReloadRequest>::Failure(
                    SourceError("application.shader_reload.source_invalid",
                                "Capture sources against an explicitly accepted host publication fence."));
            if (auto valid = ValidateRecipe(recipe); valid.HasError())
                return Result<ShaderHotReloadRequest>::Failure(valid.ErrorValue());
            return CaptureFamily(recipe, fence, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<ShaderHotReloadRequest>::Failure(
                SourceError("application.shader_reload.source_capacity", "Source capture exceeded available bounded storage."));
        } catch (const std::filesystem::filesystem_error &) {
            return Result<ShaderHotReloadRequest>::Failure(
                SourceError("application.shader_reload.source_read", "Native path admission failed before shader publication."));
        }
    }
}  // namespace Horo::Application::Internal
