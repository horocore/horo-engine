#include <Horo/Audio/AudioSpatialModel.h>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Audio::AudioSpatialFrame>);
static_assert(std::is_trivially_copyable_v<Horo::Audio::AudioSpatialMotion>);
static_assert(std::is_same_v<decltype(Horo::Audio::AudioSpatialPose::orientation), Horo::Math::Quaternion>);
