#pragma once
/** @file LuaBehaviorMetadata.h
 * @brief Target-private inert metadata reads shared by behavior and replication compilation.
 */
#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Borrowed immutable metadata evidence for one validated Lua program publication; retains no VM or runtime. */
    struct LuaBehaviorReloadMetadata final {
        const BehaviorDescriptor &descriptor;
        const std::optional<GameplayReplicationRegistration> &replication;
        const ReplicationRegistrationRegistry *replicationRegistry;
        const std::string &sourceName;
    };

    /** @brief Validates descriptor, replication ownership, schedule and codecs before any source publication or scope revocation.
     * @param active Current immutable publication. @param candidate Replacement, or missing.
     * @return Existing typed compatibility failure with original source context, or success without mutation. */
    [[nodiscard]] Result<void> ValidateLuaBehaviorReplacement(const LuaBehaviorReloadMetadata &active,
                                                              const LuaBehaviorReloadMetadata *candidate);
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
