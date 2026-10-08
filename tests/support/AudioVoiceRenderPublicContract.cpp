#include "Horo/Audio/AudioVoiceRenderRuntime.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioVoiceRenderRuntime>);
static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioStreamRenderPort>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Audio::AudioStreamRenderPort>);
static_assert(std::is_trivially_destructible_v<Horo::Audio::AudioPublishVoiceStateCommand>);

namespace {
    template <typename Runtime>
    concept CanBypassVoicePreparation = requires { Runtime({}, nullptr); };
    static_assert(!CanBypassVoicePreparation<Horo::Audio::AudioVoiceRenderRuntime>);
}  // namespace
