#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Render::PreparedVfxFrameSlot>);
static_assert(!std::is_copy_constructible_v<Horo::Render::PreparedVfxFrameSlot>);
static_assert(std::is_member_function_pointer_v<decltype(&Horo::Render::RenderFrontend::PrepareVfxFrameSlot)>);
static_assert(std::is_member_function_pointer_v<decltype(&Horo::Render::RenderFrontend::UpdateVfxFrame)>);
