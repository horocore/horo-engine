#include "Horo/Runtime/Render/RenderFrontend.h"

#include <type_traits>

static_assert(std::is_member_function_pointer_v<decltype(&Horo::Render::RenderFrameScope::PrepareParallelGraphExecution)>);
static_assert(!std::is_copy_constructible_v<Horo::Render::RenderFrameScope>);
static_assert(std::is_move_constructible_v<Horo::Render::RenderFrameScope>);

using CpuPreparation = Horo::Result<void> (Horo::Render::RenderFrameScope::*)(const Horo::JobSystem &,
                                                                              std::span<const Horo::Render::RenderPassDescriptor>,
                                                                              const Horo::Render::RenderParallelWorkLimits &,
                                                                              const Horo::CancellationToken &);
using NativePreparation = Horo::Result<void> (Horo::Render::RenderFrameScope::*)(const Horo::JobSystem &,
                                                                                 const Horo::Render::CompiledRenderGraphExecution &,
                                                                                 std::span<const Horo::Render::RenderGraphPassWorkload>,
                                                                                 const Horo::CancellationToken &) noexcept;
static_assert(std::is_same_v<decltype(&Horo::Render::RenderFrameScope::PrepareParallelExecution), CpuPreparation>);
static_assert(std::is_same_v<decltype(&Horo::Render::RenderFrameScope::PrepareParallelGraphExecution), NativePreparation>);
