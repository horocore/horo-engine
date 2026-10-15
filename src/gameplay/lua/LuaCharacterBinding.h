#pragma once
#include "Horo/Gameplay/GameplayCharacterContext.h"

#include <array>
struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief Fixed weak scope inventory revoked synchronously on compatible script replacement and program destruction. */
    class LuaCharacterScopeRegistry final {
    public:
        LuaCharacterScopeRegistry() = default;
        LuaCharacterScopeRegistry(const LuaCharacterScopeRegistry &) = delete;
        LuaCharacterScopeRegistry &operator=(const LuaCharacterScopeRegistry &) = delete;
        ~LuaCharacterScopeRegistry();
        /** @brief Retains no strong consumer ownership. @param scope Newly issued VM scope. @return False at fixed capacity. */
        [[nodiscard]] bool Retain(const std::shared_ptr<const GameplayCharacterContext> &scope);
        /** @brief Revokes every admitted VM scope without calling a VM, Scene or world. */
        void Revoke() noexcept;

    private:
        std::array<std::weak_ptr<const GameplayCharacterContext>, Character::MaximumCharacterCapabilitiesPerWorld> scopes_;
    };

    /** @brief Pushes a bounded Character projection retaining only owned grants/errors, never BehaviorContext or world pointers.
     * @param state Current VM. @param scope VM-owned child grant. @param error Original scope admission failure, if any. */
    void PushLuaCharacterContext(lua_State *state, std::shared_ptr<const GameplayCharacterContext> scope,
                                 const std::optional<Error> &error);
    /** @brief Reads the complete closed movement payload using raw table storage, preserving unsigned tick/correlation exactly.
     * @param state Current VM. @param index Raw request table. @param handle Exact owned controller handle.
     * @param physicsWorld Paired world used only for selector identity validation. @return Owned command or typed malformed failure. */
    [[nodiscard]] Result<Character::CharacterMovementRequest> ReadLuaCharacterMovement(lua_State *state, int index,
                                                                                       Character::CharacterControllerHandle handle,
                                                                                       Physics::PhysicsWorldId physicsWorld);
    /** @brief Writes a lossless decimal uint64 value. @param state VM. @param value Copied identity/tick/correlation. */
    void PushCharacterUnsigned(lua_State *state, std::uint64_t value);
}  // namespace Horo::Gameplay::Detail
