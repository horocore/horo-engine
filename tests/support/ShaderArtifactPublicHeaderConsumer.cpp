#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Render::ShaderArtifactStageView>);
static_assert(std::is_trivially_copyable_v<Horo::Render::ShaderArtifactPackageView>);
static_assert(Horo::Render::ShaderArtifactPackageView::MaximumStages == 8);
