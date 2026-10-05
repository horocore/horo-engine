#include <Horo/Runtime/Scene/AudioSceneExtraction.h>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::AudioSceneExtractor>);
static_assert(
    std::is_same_v<decltype(std::declval<Horo::Runtime::AudioSceneExtractor &>().Capture(
                       std::declval<Horo::Runtime::RuntimeSceneView>(), std::declval<const Horo::Runtime::AudioSceneExtractionInput &>())),
                   Horo::Result<Horo::Audio::AudioSpatialFrame>>);
