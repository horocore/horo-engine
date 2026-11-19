#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/Runtime/Render/MultiviewGraphWorkload.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::Render::MultiviewSceneSnapshot>);
static_assert(std::is_trivially_copyable_v<Horo::Render::MultiviewSceneInstance>);
static_assert(Horo::Render::MultiviewShaderParameterIds::PreviousEyeDepthBindings.size() == 2);

int main() {
    const Horo::Render::MultiviewExtractionLimits limits;
    const Horo::Render::RenderGraphMultiview coverage{.output = Horo::Render::MultiviewOutput::ReprojectionCoverage};
    return limits.IsValid() && coverage.targets.empty() ? 0 : 1;
}

[[maybe_unused]] bool StageMultiviewMotionHistory(Horo::Render::MultiviewMotionHistory &history,
                                                  const Horo::Render::MultiviewMotionFrame &frame,
                                                  const Horo::Render::MultiviewSceneSnapshot &scene,
                                                  std::span<const Horo::Render::RenderMotionObjectId> objects) {
    return history.Stage(frame, scene, objects);
}
