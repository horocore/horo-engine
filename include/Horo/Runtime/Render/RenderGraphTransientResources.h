#pragma once

/**
 * @file RenderGraphTransientResources.h
 * @brief Generation-safe identity of frontend-owned realized graph backing.
 */

#include "Horo/Runtime/Render/RenderGraph.h"

#include <compare>
#include <cstdint>

namespace Horo::Render {
    /**
     * @brief Identifies one completely admitted transient resource set owned by a frontend.
     *
     * The value owns no native resources. The frontend owns its backing until explicit
     * release or shutdown; accepted submissions retain that backing until actual completion.
     * A set cannot be reused while its preceding submission is in flight. Identities never
     * retarget after release, renderer restart, or graph replacement.
     */
    struct RenderGraphTransientResourcesHandle {
        RenderResourceOwnerId renderer;
        RenderGraphOwnerId graph;
        std::uint64_t value{0};

        /** @brief Reports whether the identity is structurally valid. @return True for non-zero owner identities and sequence. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && graph.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphTransientResourcesHandle &) const noexcept = default;
    };
}  // namespace Horo::Render
