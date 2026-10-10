#include "Horo/Runtime/Render/LightFrameBufferPool.h"

#include <type_traits>

namespace {
    /** @brief Consumers cannot form the factory key through empty braced initialization. */
    template <typename Pool>
    concept AllowsUnpreparedConstruction = requires(Horo::Render::RenderFrontend &frontend,
                                                    const Horo::Render::LightCullingBudget &budget) { Pool{frontend, budget, 1U, {}}; };

    static_assert(!AllowsUnpreparedConstruction<Horo::Render::LightFrameBufferPool>);
    static_assert(!std::is_constructible_v<Horo::Render::LightFrameBufferPool, Horo::Render::RenderFrontend &,
                                           const Horo::Render::LightCullingBudget &, std::uint32_t>);
}  // namespace
