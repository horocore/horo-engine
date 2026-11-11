#include "Horo/Accessibility/CaptionTransport.h"

#include <type_traits>

static_assert(std::is_trivially_copyable_v<Horo::Accessibility::CaptionCue>);
static_assert(std::is_trivially_destructible_v<Horo::Accessibility::CaptionCue>);

int main() {
    return Horo::Accessibility::MaximumCaptionCues == 32 ? 0 : 1;
}
