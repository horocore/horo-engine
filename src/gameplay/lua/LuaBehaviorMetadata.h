#pragma once
/** @file LuaBehaviorMetadata.h
 * @brief Target-private inert metadata reads shared by behavior and replication compilation.
 */
#include "Horo/Gameplay/BehaviorRegistry.h"
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Pushes one raw metadata value from the table at the top of the Lua stack.
     * @param state Compiler-owned Lua state with a table on top.
     * @param key Borrowed literal metadata key.
     * @param coerceString Preserves existing behavior numeric-to-string metadata coercion within the protected call.
     * @throws std::invalid_argument When Lua cannot intern the key within its compilation budget.
     */
    void ReadLuaMetadataField(lua_State *state, const char *key, bool coerceString = false);
    /** @brief Reads the existing typed behavior declaration without executing author metamethods.
     * @param state Compiler-owned state holding the returned behavior table.
     * @param canonicalTypeId Sidecar-owned canonical behavior identity.
     * @return Owned behavior descriptor or malformed metadata diagnostics.
     */
    [[nodiscard]] Result<BehaviorDescriptor> ReadLuaBehaviorDescriptor(lua_State *state, const BehaviorTypeId &canonicalTypeId);
}  // namespace Horo::Gameplay::Detail
