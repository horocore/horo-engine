#include "Horo/Runtime/InputCollection.h"

#include <type_traits>
#include <utility>
static_assert(!std::is_copy_constructible_v<Horo::Input::InputActionSourceToken>);
static_assert(!std::is_copy_constructible_v<Horo::Input::InputCollectionFrame>);
static_assert(std::is_move_constructible_v<Horo::Input::InputCollectionFrame>);
static_assert(!std::is_move_constructible_v<Horo::Input::InputRouter>);

static_assert(Horo::Input::MaximumCollectedInputActions == 256);
static_assert(std::is_same_v<decltype(std::declval<const Horo::Input::InputActionSourceToken &>().Validate()), Horo::Result<void>>);
