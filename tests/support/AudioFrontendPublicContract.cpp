#include "Horo/Audio/AudioFrontend.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioFrontend>);
static_assert(!std::is_move_constructible_v<Horo::Audio::AudioFrontend>);
static_assert(std::is_copy_constructible_v<Horo::Audio::AudioFrontendSnapshot>);
