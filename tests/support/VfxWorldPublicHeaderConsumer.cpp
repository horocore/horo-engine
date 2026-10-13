#include "Horo/Vfx/VfxWorld.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Vfx::VfxWorld>);
static_assert(std::is_copy_constructible_v<Horo::Render::RenderWorldSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<Horo::Render::RenderWorldSnapshot>);

static_assert(
    noexcept(std::declval<const Horo::Vfx::CpuParticleBuffer &>().TryReadView(std::declval<Horo::Vfx::CpuParticleSoAConstView &>())));
static_assert(noexcept(std::declval<const Horo::Vfx::CpuParticleBuffer &>().TryHandleAt(0,
                                                                                        std::declval<Horo::Vfx::CpuParticleHandle &>())));
