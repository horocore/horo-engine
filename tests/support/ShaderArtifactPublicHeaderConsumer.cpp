#include "Horo/Runtime/Render/RenderShaderResources.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Render::ShaderArtifactStageView>);
static_assert(std::is_trivially_copyable_v<Horo::Render::ShaderArtifactPackageView>);
static_assert(Horo::Render::ShaderArtifactPackageView::MaximumStages == 8);

/** @brief Exercises runtime shader contracts using only their RenderApi owner's staged usage requirements. */
[[maybe_unused]] Horo::Result<void> ConsumeShaderDescriptor(const Horo::Render::RenderShaderModuleDescriptor &descriptor) {
    return Horo::Render::ValidateRenderShaderModuleDescriptor(descriptor);
}

static_assert(std::is_same_v<decltype(Horo::Render::CompiledShaderArtifact{}.reflection),
                             std::shared_ptr<const Horo::Render::ReflectedShaderArtifact>>);
static_assert(std::is_same_v<decltype(Horo::Render::ShaderCompilationRequest{}.reflection),
                             std::optional<Horo::Render::ShaderCompilerReflectionRequest>>);
