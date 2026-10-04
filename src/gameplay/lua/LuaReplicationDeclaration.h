#pragma once

#include "Horo/Gameplay/ReplicationRegistration.h"

#include <optional>

struct lua_State;

namespace Horo::Gameplay::Detail {
    /**
     * @brief Generates a typed declaration from the returned source table without retaining Lua values.
     * @param state Compilation VM with the returned behavior table on top; its stack is preserved.
     * @param owner Sidecar-owned behavior identity.
     * @param schema Sidecar-owned schema identity, invalid only when no replication is declared.
     * @param moduleId Sidecar-owned gameplay module identity.
     * @return Generated contribution, no declaration, or a typed metadata/codec error.
     */
    [[nodiscard]] Result<std::optional<GameplayReplicationRegistration>> ReadLuaReplicationDeclaration(lua_State *state,
                                                                                                       const BehaviorTypeId &owner,
                                                                                                       Network::ReplicationSchemaId schema,
                                                                                                       const ModuleId &moduleId);
    /** @brief Freezes a generated declaration through the common native registry.
     * @param declaration Generated typed contribution.
     * @param descriptor Matching canonical behavior descriptor.
     * @return Owning frozen registry or typed validation diagnostics.
     */
    [[nodiscard]] Result<std::unique_ptr<ReplicationRegistrationRegistry>> BuildLuaReplicationRegistry(
        const GameplayReplicationRegistration &declaration, const BehaviorDescriptor &descriptor);
    /** @brief Rejects reinterpretation of an existing codec identity during safe-point reload.
     * @param active Previously published declaration.
     * @param replacement Validated candidate declaration.
     * @param sourceName Candidate source context for diagnostics.
     * @return Success or incompatible codec diagnostics.
     */
    [[nodiscard]] Result<void> ValidateLuaReplicationCodecs(const GameplayReplicationRegistration &active,
                                                            const GameplayReplicationRegistration &replacement,
                                                            const std::string &sourceName);
}  // namespace Horo::Gameplay::Detail
