#include "Horo/Runtime/Render/RenderFrontend.h"

#include <type_traits>

using SnapshotResult = Horo::Result<std::shared_ptr<const Horo::Render::RenderGraphInspectionSnapshot>>;
using Capture = SnapshotResult (Horo::Render::RenderFrameScope::*)(const Horo::Render::RenderGraph &,
                                                                   const Horo::Render::RenderGraphSchedule &,
                                                                   const Horo::Render::RenderGraphLifetimePlan &,
                                                                   const Horo::Render::CompiledRenderGraphExecution &,
                                                                   Horo::Render::RenderGraphInspectionLimits, std::stop_token);
using Read = SnapshotResult (Horo::Render::RenderFrontend::*)() const;
static_assert(std::is_same_v<decltype(&Horo::Render::RenderFrameScope::CaptureInspection), Capture>);
static_assert(std::is_same_v<decltype(&Horo::Render::RenderFrontend::GraphInspectionSnapshot), Read>);
static_assert(!std::is_copy_constructible_v<Horo::Render::RenderGraphInspectionSnapshot>);
