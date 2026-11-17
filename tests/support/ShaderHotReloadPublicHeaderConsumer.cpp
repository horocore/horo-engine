#include "Horo/Application/ShaderHotReloadService.h"

#include <type_traits>

using ShaderReloadRequestSignature = Horo::Result<Horo::Application::ShaderHotReloadAdmission> (
    Horo::Application::ShaderHotReloadService::*)(const Horo::Application::ShaderHotReloadRequest &, const Horo::CancellationToken &);
static_assert(std::is_same_v<decltype(&Horo::Application::ShaderHotReloadService::Request), ShaderReloadRequestSignature>);
static_assert(!std::is_copy_constructible_v<Horo::Application::ShaderHotReloadService>);
static_assert(std::has_virtual_destructor_v<Horo::Render::IShaderHotReloadCandidate>);
static_assert(std::is_same_v<decltype(Horo::Application::ShaderHotReloadProgram{}.id), Horo::Render::ShaderHotReloadProgramId>);
static_assert(std::is_same_v<decltype(Horo::Render::ShaderHotReloadProgramView{}.compiled), const Horo::Render::ShaderCompilationBatch *>);
