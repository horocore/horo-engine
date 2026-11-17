#include <Horo/Audio/AudioDeviceDiscovery.h>
#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<Horo::Audio::AudioDeviceClass>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(Horo::Audio::AudioDeviceClass::Physical) == 0);
static_assert(static_cast<std::uint8_t>(Horo::Audio::AudioDeviceClass::Virtual) == 1);
static_assert(static_cast<std::uint8_t>(Horo::Audio::AudioDeviceClass::Headless) == 2);
static_assert(static_cast<std::uint8_t>(Horo::Audio::AudioDeviceClass::Unknown) == 3);
