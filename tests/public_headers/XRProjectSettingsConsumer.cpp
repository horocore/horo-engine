#include "Horo/XR/XRProjectSettings.h"
#include "Horo/XR/XRSessionLifecycle.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::XR::XRProjectSettings>);
static_assert(!std::is_default_constructible_v<Horo::XR::XRProjectSettings>);
static_assert(static_cast<std::uint8_t>(Horo::XR::XRSessionEvent::InstanceLost) == 8);
static_assert(static_cast<std::uint8_t>(Horo::XR::XRSessionEvent::Ready) == 9);

void ConsumeXRProjectSettings(const Horo::XR::XRProjectSettings &settings) {
    [[maybe_unused]] const auto revision = settings.Revision();
    [[maybe_unused]] const auto features = settings.Features();
    [[maybe_unused]] const auto &limits = settings.Limits();
}
