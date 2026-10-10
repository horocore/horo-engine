#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionFeed>);
static_assert(std::is_same_v<decltype(&Horo::Render::RenderGraphInspectionSnapshot::Timing),
                             Horo::Render::RenderGraphInspectionTiming (Horo::Render::RenderGraphInspectionSnapshot::*)() const noexcept>);
