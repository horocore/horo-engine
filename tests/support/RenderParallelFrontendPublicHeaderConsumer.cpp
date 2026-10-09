#include "Horo/Runtime/Render/RenderFrontend.h"

#include <type_traits>

static_assert(std::is_member_function_pointer_v<decltype(&Horo::Render::RenderFrameScope::PrepareParallelGraphExecution)>);
static_assert(!std::is_copy_constructible_v<Horo::Render::RenderFrameScope>);
static_assert(std::is_move_constructible_v<Horo::Render::RenderFrameScope>);
