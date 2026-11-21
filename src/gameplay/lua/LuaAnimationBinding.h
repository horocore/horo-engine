#pragma once

#include "Horo/Gameplay/GameplayAnimationContext.h"

struct lua_State;

namespace Horo::Gameplay::Detail {
    /** @brief VM-owned callback admission. An epoch is never reused within one Lua state. */
    struct LuaAnimationCallbackState final {
        std::uint64_t epoch{};
        bool active{};
    };

    /** @brief Begins admission outside the protected Lua call and revokes it on every return path. */
    class LuaAnimationCallback final {
    public:
        explicit LuaAnimationCallback(LuaAnimationCallbackState &state) noexcept;
        ~LuaAnimationCallback();
        LuaAnimationCallback(const LuaAnimationCallback &) = delete;
        LuaAnimationCallback &operator=(const LuaAnimationCallback &) = delete;

    private:
        LuaAnimationCallbackState &state_;
    };

    /** @brief Pushes self-scoped animation functions with exact callback and actor admission. */
    void PushLuaAnimationContext(lua_State *state, const std::shared_ptr<const GameplayAnimationContext> &context, GameplayEntityRef entity,
                                 const LuaAnimationCallbackState &callback);
}  // namespace Horo::Gameplay::Detail
