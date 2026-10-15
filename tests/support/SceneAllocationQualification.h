#pragma once

#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests {
    /** @brief Refuses unsupported MSVC Debug proxy injection; exact probes remain mandatory in Windows Release CI.
     * @details Debug iterator proxies allocate inside noexcept moves, including returned Result containers. Those allocation
     * sites can occur after the operation's cancellation linearization point. Injecting there cannot qualify the production
     * throwable-allocation contract. No allocator is exempted and every ordinary Debug Scene case remains enabled.
     */
    inline void RequireSupportedSceneAllocationInjection() {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        SKIP("MSVC Debug iterator-proxy injection is unsupported; mandatory Windows Release qualification covers this exact case.");
#endif
    }
}  // namespace Horo::Tests
