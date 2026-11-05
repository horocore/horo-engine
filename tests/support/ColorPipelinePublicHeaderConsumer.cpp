#include "Horo/Runtime/Render/ColorPipeline.h"
#include "Horo/Runtime/Render/Exposure.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"

#include <type_traits>

static_assert(sizeof(Horo::Render::ExposureDispatchParameters) == 80);
static_assert(alignof(Horo::Render::ExposureDispatchParameters) == 16);
static_assert(sizeof(Horo::Render::ExposureGpuState) == 32);
static_assert(sizeof(Horo::Render::ExposureGpuDispatchParameters) == 96);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::Render::ColorPipelinePlan &>().Inputs()), const Horo::Render::ColorPipelineRequest &>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::CookedColorTransform &>().Rgb()), std::span<const float>>);
