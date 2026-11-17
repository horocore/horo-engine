#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"

#include <type_traits>

using InspectionCapture = Horo::Result<std::shared_ptr<const Horo::Render::RenderGraphInspectionSnapshot>> (*)(
    const Horo::Render::RenderGraph &, const Horo::Render::RenderGraphSchedule &, const Horo::Render::RenderGraphLifetimePlan &,
    const Horo::Render::CompiledRenderGraphExecution &, Horo::Render::RenderGraphInspectionContext,
    Horo::Render::RenderGraphInspectionLimits, Horo::CancellationToken);
using InspectionExport = Horo::Result<std::string> (*)(const Horo::Render::RenderGraphInspectionSnapshot &, std::size_t,
                                                       Horo::CancellationToken);
static_assert(std::is_same_v<decltype(&Horo::Render::CaptureRenderGraphInspection), InspectionCapture>);
static_assert(std::is_same_v<decltype(&Horo::Render::ExportRenderGraphInspection), InspectionExport>);

static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionFeed>);
static_assert(!std::is_default_constructible_v<Horo::Render::RenderGraphInspectionSnapshot>);
static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionSnapshot>);
template <typename Snapshot>
concept AllowsForgedInspectionConstruction = requires { Snapshot({}); };
static_assert(!AllowsForgedInspectionConstruction<Horo::Render::RenderGraphInspectionSnapshot>);
static_assert(std::is_same_v<decltype(&Horo::Render::RenderGraphInspectionSnapshot::Timing),
                             Horo::Render::RenderGraphInspectionTiming (Horo::Render::RenderGraphInspectionSnapshot::*)() const noexcept>);
