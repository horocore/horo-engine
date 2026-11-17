#include "RuntimeUiPaintProjectKernel.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <bit>
#include <fstream>

namespace Horo::Application::Internal {
    namespace {
        using Preparation = Result<std::shared_ptr<const Render::CookedUiPaintKernel>>;

        /** @brief Bounds source bytes before allocation; this is an explicit application load-time filesystem boundary. */
        Result<std::vector<std::uint8_t>> ReadSource(const std::filesystem::path &path) {
            std::ifstream file{path, std::ios::binary | std::ios::ate};
            const auto size = file ? file.tellg() : std::streampos{-1};
            if (size <= 0 || size > 1024 * 1024)
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            if (!file.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc PrepareProjectUiKernel */
    Result<std::shared_ptr<const Render::CookedUiPaintKernel>> PrepareProjectUiKernel(const RuntimeUiPaintProjectCompiler &compiler,
                                                                                      const CancellationToken &cancellation) {
        using namespace Render;
        if (compiler.target.requirement.backend != ShaderTargetBackend::Metal)
            return Preparation::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
        const auto cross = std::ranges::find(compiler.target.tools, ShaderCompilerTool::SpirvCross, &ShaderCompilerToolIdentity::tool);
        if (cross == compiler.target.tools.end())
            return Preparation::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
        auto source = ReadSource(compiler.source);
        if (source.HasError())
            return Preparation::Failure(source.ErrorValue());
        ShaderBuildRequest request;
        request.compilation.manifest = MakeUiPaintShaderManifest(compiler.target.requirement);
        request.compilation.source = std::move(source).Value();
        request.compilation.targets = {compiler.target};
        request.compilation.reflection = ShaderCompilerReflectionRequest{*cross,
                                                                         {{{1}, "PaintParameters", {}},
                                                                          {{2}, "PaintTexture", {}},
                                                                          {{3}, "CoverageTexture", {}},
                                                                          {{4}, "PaintSampler", {}}}};

        struct Work final {
            ShaderBuildRequest request;
            std::optional<Result<ShaderCompilationBatch>> result;
        };

        auto work = std::make_shared<Work>(Work{std::move(request), {}});
        auto submitted = compiler.jobs.get().SubmitResult({.parentCancellation = cancellation},
                                                          [work, &builds = compiler.builds.get()](const CancellationToken &token) {
            work->result.emplace(builds.Compile(work->request, token));
            return work->result->HasError() ? Result<void>::Failure(work->result->ErrorValue()) : Result<void>::Success();
        });
        if (submitted.HasError())
            return Preparation::Failure(submitted.ErrorValue());
        if (const auto joined = submitted.Value().Wait(); joined.HasError())
            return Preparation::Failure(joined.ErrorValue());
        if (!work->result || work->result->HasError() || work->result->Value().artifacts.size() != 1)
            return Preparation::Failure(work->result && work->result->HasError()
                                            ? work->result->ErrorValue()
                                            : MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        auto compiled = std::move(*work->result).Value();
        auto kernel = std::make_shared<CookedUiPaintKernel>(std::move(work->request.compilation.manifest), compiler.target.requirement,
                                                            std::move(compiled.artifacts.front()), RenderTextureFormat::Bgra8Unorm,
                                                            RenderTextureFormat::Depth32FloatStencil8);
        if (const auto validated = ValidateCookedUiPaintKernel(*kernel, ShaderTargetBackend::Metal); validated.HasError())
            return Preparation::Failure(validated.ErrorValue());
        return Preparation::Success(std::move(kernel));
    }
}  // namespace Horo::Application::Internal
