#include "Horo/Audio/AudioVoiceRenderRuntime.h"

#include <optional>
#include <type_traits>
#include <utility>

static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioVoiceRenderRuntime>);
static_assert(!std::is_copy_constructible_v<Horo::Audio::AudioStreamRenderPort>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Audio::AudioStreamRenderPort>);
static_assert(std::is_trivially_destructible_v<Horo::Audio::AudioPublishVoiceStateCommand>);
static_assert(noexcept(std::declval<const Horo::Audio::AudioVoiceRenderRuntime &>().TerminalSnapshot()));
static_assert(std::is_same_v<decltype(std::declval<const Horo::Audio::AudioVoiceRenderRuntime &>().TerminalSnapshot()),
                             std::optional<Horo::Audio::AudioVoiceSnapshot>>);

namespace {
    template <typename Runtime>
    concept CanBypassVoicePreparation = requires { Runtime({}, nullptr); };
    static_assert(!CanBypassVoicePreparation<Horo::Audio::AudioVoiceRenderRuntime>);
}  // namespace
