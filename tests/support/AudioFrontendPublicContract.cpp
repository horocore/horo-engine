#include "Horo/Audio/AudioFrontend.h"

#include <optional>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioFrontend>);
static_assert(!std::is_move_constructible_v<Horo::Audio::AudioFrontend>);
static_assert(std::is_copy_constructible_v<Horo::Audio::AudioFrontendSnapshot>);
static_assert(std::is_same_v<decltype(Horo::Audio::AudioFrontendSnapshot{}.terminalVoice), std::optional<Horo::Audio::AudioVoiceSnapshot>>);
