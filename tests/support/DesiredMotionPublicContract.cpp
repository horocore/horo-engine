#include "Horo/CharacterInput/DesiredMotionAdapter.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::CharacterInput::DesiredMotionAdapter>);
static_assert(std::is_nothrow_move_constructible_v<Horo::CharacterInput::DesiredMotionAdapter>);
static_assert(std::is_copy_constructible_v<Horo::CharacterInput::DesiredMotionFrame>);
static_assert(
    std::is_same_v<decltype(Horo::CharacterInput::DesiredMotionFrame{}.Intent()), const Horo::CharacterInput::DesiredMotionIntent &>);
