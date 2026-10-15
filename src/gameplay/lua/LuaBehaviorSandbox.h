#pragma once
/** @file LuaBehaviorSandbox.h
 * @brief Target-private standard-library admission for the existing behavior VM sandbox.
 */
#include <cstddef>

struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Existing per-VM accounting; the Lua state borrows this owner until lua_close completes. */
    struct LuaBudget final {
        std::size_t used{};
        std::size_t maximum{};
    };

    /** @brief Existing Lua realloc/free ABI with exact bounded per-VM accounting; no global allocator authority.
     * @param userData Borrowed LuaBudget outliving the VM. @param pointer Existing storage or null.
     * @param oldSize Lua allocation size, or its object-type tag for a fresh allocation.
     * @param newSize Requested bytes; zero releases storage. @return Resized memory or null without changing failed accounting.
     */
    void *BudgetAllocate(void *userData, void *pointer, std::size_t oldSize, std::size_t newSize);
    /** @brief Install the existing restricted library set and inert behavior declaration function.
     * @param state Explicit compiler/runtime-owned VM; caller owns its Lua allocation/error boundary.
     */
    void OpenLuaBehaviorSandbox(lua_State *state);
}  // namespace Horo::Gameplay::Detail
