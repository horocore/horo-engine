#include "Horo/Application/ShaderBuildService.h"

#include <type_traits>

using ShaderBuildCompileSignature = Horo::Result<Horo::Render::ShaderCompilationBatch> (Horo::Application::ShaderBuildService::*)(
    const Horo::Application::ShaderBuildRequest &, const Horo::CancellationToken &) const;
static_assert(std::is_same_v<decltype(&Horo::Application::ShaderBuildService::Compile), ShaderBuildCompileSignature>);

using CompileSignature = Horo::Result<Horo::Render::ShaderCompilationBatch> (*)(const Horo::Render::ShaderCompilationRequest &,
                                                                                const Horo::Render::IShaderCompilerAdapter &,
                                                                                const Horo::CancellationToken &,
                                                                                const Horo::Render::ShaderCompilerLimits &,
                                                                                Horo::Render::IShaderCompilerDiagnosticSink *);
static_assert(std::is_same_v<decltype(&Horo::Render::CompileShaderTargets), CompileSignature>);
static_assert(!std::is_copy_constructible_v<Horo::Application::ShaderBuildService>);
static_assert(std::is_same_v<decltype(Horo::Application::ShaderBuildRequest{}.operationId), std::optional<Horo::OperationId>>);
