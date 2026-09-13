#pragma once

#include "Horo/Runtime/Render/RenderMemoryTypes.h"

namespace Horo::Editor::RenderMemoryScopes {
    inline constexpr Render::RenderMemoryScopeId HostResources{100, 1};
    inline constexpr Render::RenderMemoryScopeId ViewportResources{101, 1};
    inline constexpr Render::RenderMemoryScopeId GuiResources{102, 1};
}  // namespace Horo::Editor::RenderMemoryScopes
