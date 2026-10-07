#include <Horo/Audio/CoreStereoSpatialRenderer.h>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Audio::AudioStereoSpatialSettings>);
static_assert(std::is_trivially_copyable_v<Horo::Audio::AudioStereoSpatialTarget>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Audio::CoreStereoSpatialRenderer>);
static_assert(!std::is_copy_constructible_v<Horo::Audio::CoreStereoSpatialRenderer>);
