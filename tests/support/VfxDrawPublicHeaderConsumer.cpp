#include "Horo/Runtime/Render/VfxDrawData.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Render::VfxPackedDrawInstance>);
static_assert(sizeof(Horo::Render::VfxDrawUniformBlock) == 10'240);
static_assert(alignof(Horo::Render::VfxPackedDrawInstance) == 16);
static_assert(
    std::is_same_v<decltype(Horo::Render::MakeVfxShaderManifest(Horo::Render::ShaderTargetRequirement{})), Horo::Render::ShaderManifest>);
