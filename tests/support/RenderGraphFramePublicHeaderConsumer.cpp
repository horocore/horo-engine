#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <type_traits>
#include <utility>

static_assert(std::is_same_v<decltype(std::declval<Horo::Render::RenderFrameScope &>().ExecuteGraph(
                                 std::declval<const Horo::Render::CompiledRenderGraphExecution &>(),
                                 std::span<const Horo::Render::RenderGraphPassWorkload>{})),
                             Horo::Result<void>>);
