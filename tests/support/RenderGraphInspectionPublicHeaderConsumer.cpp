#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionFeed>);
static_assert(!std::is_default_constructible_v<Horo::Render::RenderGraphInspectionSnapshot>);
static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionSnapshot>);
template <typename Snapshot>
concept AllowsForgedInspectionConstruction = requires { Snapshot({}); };
static_assert(!AllowsForgedInspectionConstruction<Horo::Render::RenderGraphInspectionSnapshot>);
static_assert(std::is_same_v<decltype(&Horo::Render::RenderGraphInspectionSnapshot::Timing),
                             Horo::Render::RenderGraphInspectionTiming (Horo::Render::RenderGraphInspectionSnapshot::*)() const noexcept>);
