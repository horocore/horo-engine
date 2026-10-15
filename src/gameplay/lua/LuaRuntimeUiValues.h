#pragma once
/** @file LuaRuntimeUiValues.h
 * @brief Closed lossless Lua Runtime UI value transport; no arbitrary property maps or metamethod reads.
 */
#include "Horo/Runtime/Ui/RuntimeUiClient.h"

#include <charconv>
struct lua_State;

namespace Horo::Gameplay::Detail::UiScript {
    using Runtime::Ui::SerializedUiId;
    using Runtime::Ui::UiBindingValue;
    inline constexpr const char *EventType = "horo.runtime_ui.event";
    inline constexpr const char *TerminalType = "horo.runtime_ui.terminal";
    inline constexpr const char *RouteType = "horo.runtime_ui.route";
    inline constexpr const char *RequestType = "horo.runtime_ui.request";
    inline constexpr const char *ClientType = "horo.runtime_ui.client";
    inline constexpr const char *ElementType = "horo.runtime_ui.element";
    inline constexpr const char *SourceType = "horo.runtime_ui.source";
    inline constexpr const char *GuardType = "horo.runtime_ui.guard";
    inline constexpr const char *ValueType = "horo.runtime_ui.value";
    inline constexpr const char *ActionValueType = "horo.runtime_ui.action_value";
    /** @brief POD-only inner marshalling of exact uint64; call only inside Protected or the protected context constructor. */
    void PushUnsigned(lua_State *state, std::uint64_t value);
    /** @brief Reads raw decimal strings or nonnegative Lua integers only. */
    [[nodiscard]] bool ReadUnsigned(lua_State *state, int index, std::uint64_t &value) noexcept;
    /** @brief Reads exactly 32 hexadecimal identity bytes without coercion or metamethods. */
    [[nodiscard]] Result<SerializedUiId> ReadIdentity(lua_State *state, int index);
    /** @brief Copies canonical error identity into a nil/error pair. */
    int Failure(lua_State *state, const Error &error);
    /** @brief Returns a typed malformed/unsupported pair. */
    int Invalid(lua_State *state);
    /** @brief Installs closed binding/action value factories and value unpack methods on the top method table. */
    void InstallValueFactories(lua_State *state);
    /** @brief Pushes an owned binding value, with no provider callbacks or borrowed strings. */
    void PushBindingValue(lua_State *state, const UiBindingValue &value);
}  // namespace Horo::Gameplay::Detail::UiScript
