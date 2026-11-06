#include "Horo/Runtime/Render/ColorOutputProgram.h"
#include "Horo/Runtime/Render/ColorPipeline.h"
#include "Horo/Runtime/Render/Exposure.h"
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"
#include "Horo/Runtime/Render/SceneColorEncoding.h"

#include <type_traits>

static_assert(sizeof(Horo::Render::ExposureDispatchParameters) == 80);
static_assert(alignof(Horo::Render::ExposureDispatchParameters) == 16);
static_assert(sizeof(Horo::Render::ExposureGpuState) == 32);
static_assert(sizeof(Horo::Render::ExposureGpuDispatchParameters) == 96);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::Render::ColorPipelinePlan &>().Inputs()), const Horo::Render::ColorPipelineRequest &>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::CookedColorTransform &>().Rgb()), std::span<const float>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::CookedColorOutputProgram &>().Source()), std::string_view>);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::Render::CookedColorOutputProgram &>().TableUpload(0)), std::span<const std::byte>>);
static_assert(std::is_same_v<decltype(Horo::Render::ColorPipelineRequest::outputTransform),
                             std::shared_ptr<const Horo::Render::CookedColorOutputProgram>>);
static_assert(!std::is_convertible_v<std::shared_ptr<const Horo::Render::CookedColorTransform>,
                                     std::shared_ptr<const Horo::Render::CookedColorOutputProgram>>);
static_assert(Horo::Render::CookedColorOutputProgram::TableCount == 2);
static_assert(std::is_trivially_copyable_v<Horo::Render::SceneColorSourcePolicy>);
static_assert(std::is_same_v<decltype(Horo::Render::PrepareColorTransformSelection(
                                 std::declval<const std::shared_ptr<const Horo::Render::CookedColorOutputProgram> &>(),
                                 Horo::Render::ColorLookMode::Disabled,
                                 std::declval<const std::shared_ptr<const Horo::Render::CookedColorTransform> &>())),
                             Horo::Result<Horo::Render::ColorTransformSelection>>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::ColorTransformSelection &>().Analytic()),
                             const std::shared_ptr<const Horo::Render::CookedColorOutputProgram> &>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::ColorTransformSelection &>().Look()),
                             const std::shared_ptr<const Horo::Render::CookedColorTransform> &>);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Render::ColorTransformSelection &>().Mode()), Horo::Render::ColorLookMode>);
