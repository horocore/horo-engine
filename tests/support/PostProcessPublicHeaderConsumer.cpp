#include "Horo/Runtime/Render/PostProcessGraph.h"
#include "Horo/Runtime/Render/PostProcessVolumes.h"
#include "Horo/Runtime/Render/ReconstructionFrame.h"

#include <type_traits>

static_assert(std::is_move_constructible_v<Horo::Render::PostProcessGraphPlan>);
static_assert(!std::is_copy_constructible_v<Horo::Render::PostProcessGraphPlan>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::PostProcessGraphPlan &>().Settings()),
                             const Horo::Render::PostProcessSettings &>);
static_assert(std::is_copy_constructible_v<Horo::Render::PostProcessVolumeSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::PostProcessVolumeSnapshot &>().Evaluate(Horo::Math::Vec3{})),
                             Horo::Result<Horo::Render::PostProcessSettings>>);
static_assert(std::is_same_v<decltype(Horo::Render::ValidateReconstructionFrame(Horo::Render::ReconstructionFrameRequest{},
                                                                                Horo::Render::RenderCapabilitySnapshot{})),
                             Horo::Result<void>>);
