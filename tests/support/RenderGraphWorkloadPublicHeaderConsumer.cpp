#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <type_traits>

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphBufferCopy>);
static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphColorAttachment>);
static_assert(!std::is_copy_constructible_v<Horo::Render::CompiledRenderGraphExecution>);
