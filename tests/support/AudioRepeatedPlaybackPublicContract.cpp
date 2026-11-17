#include "Horo/Audio/AudioRepeatedPlayback.h"

#include <type_traits>

static_assert(std::is_aggregate_v<Horo::Audio::AudioRepeatedPlaybackConfig>);
static_assert(!Horo::Audio::AudioRepeatedPlaybackConfig{}.hostContext.has_value());
static_assert(requires(Horo::Audio::AudioRepeatedPlaybackConfig config, Horo::Audio::AudioSceneContextHandle context) {
    config.hostContext = context;
});
