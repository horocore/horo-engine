#include "OpenXRInputClock.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Application::Internal::OpenXRInputClock>);
static_assert(!std::is_copy_assignable_v<Horo::Application::Internal::OpenXRInputClock>);
