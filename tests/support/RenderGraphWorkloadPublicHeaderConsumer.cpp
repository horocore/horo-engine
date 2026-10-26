#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/UiPaintFrameUpload.h"
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "Horo/Runtime/Render/VfxGraphScheduling.h"

#include <type_traits>

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphBufferCopy>);
static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphColorAttachment>);
static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphLightCulling>);
static_assert(!std::is_copy_constructible_v<Horo::Render::CompiledRenderGraphExecution>);

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphVfx>);

static_assert(std::is_nothrow_move_constructible_v<Horo::Render::VfxGraphSchedulingOutcome>);
static_assert(std::is_copy_constructible_v<Horo::Render::VfxGraphSchedule>);

static_assert(std::is_constructible_v<Horo::Render::RenderGraphWorkload, Horo::Render::RenderGraphUiPaint>);
static_assert(sizeof(Horo::Render::UiPaintVertex) == sizeof(float) * 8);

static_assert(std::is_move_constructible_v<Horo::Render::UiPaintFrameUpdate>);
static_assert(sizeof(Horo::Render::UiPaintParameters) == 64);
static_assert(offsetof(Horo::Render::UiPaintParameters, maskExtent) == 32);
static_assert(offsetof(Horo::Render::UiPaintParameters, cornerRadii) == 48);
static_assert(Horo::Render::UiPaintFrameUpdate::ParameterStride == 256);
