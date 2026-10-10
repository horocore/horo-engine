#include "Horo/Runtime/Render/RenderParallelWork.h"

#include <type_traits>

static_assert(std::is_abstract_v<Horo::Render::IRenderParallelGraphRecording>);
static_assert(Horo::Render::RenderParallelWorkLimits::HardMaximumBytes == 64U * 1024U * 1024U);
static_assert(Horo::Render::RenderParallelRecordingCapabilities{}.maximumPasses == 0);
