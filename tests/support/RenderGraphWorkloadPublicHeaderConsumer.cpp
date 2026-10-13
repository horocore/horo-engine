#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/VfxGraphScheduling.h"

#include <type_traits>

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphBufferCopy>);
static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphColorAttachment>);
static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphLightCulling>);
static_assert(!std::is_copy_constructible_v<Horo::Render::CompiledRenderGraphExecution>);

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphVfx>);

static_assert(std::is_nothrow_move_constructible_v<Horo::Render::VfxGraphSchedulingOutcome>);
static_assert(std::is_copy_constructible_v<Horo::Render::VfxGraphSchedule>);
