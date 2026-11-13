#include "Horo/Extensions/LuaExtensionServiceBinding.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace Horo::Extensions {
    namespace {
        constexpr std::uint32_t HookQuantum = 1000;

        struct Execution final {
            NativeExtensionScriptSession &binding;
            const CancellationToken &cancellation;
            LuaExtensionServiceLimits limits;
            std::size_t retained{};
            std::uint32_t instructions{};
            std::string_view source;
        };

        struct CallResult final {
            std::int64_t value{};
            std::array<char, 256> error{};
            std::array<char, 128> domain{};
            std::array<char, 128> code{};
            bool success{};
        };

        void *Allocate(void *opaque, void *pointer, const std::size_t oldSize, const std::size_t newSize) noexcept {
            auto &execution = *static_cast<Execution *>(opaque);
            const auto prior = pointer ? oldSize : 0;
            if (newSize == 0) {
                execution.retained -= prior;
                std::free(pointer);
                return nullptr;
            }
            if (prior > execution.retained || newSize > execution.limits.maximumMemoryBytes - (execution.retained - prior))
                return nullptr;
            auto *next = std::realloc(pointer, newSize);
            if (next)
                execution.retained = execution.retained - prior + newSize;
            return next;
        }

        Execution &Owner(lua_State *state) noexcept {
            return **static_cast<Execution **>(lua_getextraspace(state));
        }

        void Hook(lua_State *state, lua_Debug *) {
            auto &execution = Owner(state);
            if (execution.cancellation.IsCancellationRequested())
                luaL_error(state, "extension script cancelled");
            if (execution.instructions >= execution.limits.maximumInstructions - HookQuantum)
                luaL_error(state, "extension script instruction budget exceeded");
            execution.instructions += HookQuantum;
        }

        CallResult Invoke(Execution &execution, const char *method, const std::span<const std::int64_t> arguments) noexcept {
            CallResult output;
            try {
                auto result = execution.binding.Invoke(method, arguments, execution.cancellation);
                if (result.HasValue()) {
                    output.value = result.Value();
                    output.success = true;
                } else {
                    const auto *typed = &result.ErrorValue();
                    while (typed->cause.Get())
                        typed = typed->cause.Get();
                    std::copy_n(typed->domain.Value().data(), std::min(typed->domain.Value().size(), output.domain.size() - 1),
                                output.domain.data());
                    std::copy_n(typed->code.Value().data(), std::min(typed->code.Value().size(), output.code.size() - 1),
                                output.code.data());
                    const auto &message = result.ErrorValue().message;
                    std::copy_n(message.data(), std::min(message.size(), output.error.size() - 1), output.error.data());
                }
            } catch (...) {
                std::strcpy(output.error.data(), "extension service invocation failed");
            }
            return output;
        }

        int Method(lua_State *state) {
            std::array<std::int64_t, HORO_EXTENSION_SERVICE_MAX_ARGUMENTS> arguments{};
            const auto expected = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));
            if (lua_gettop(state) != expected) {
                lua_pushnil(state);
                lua_pushliteral(state, "argument count mismatch");
                return 2;
            }
            for (int index = 0; index < expected; ++index) {
                if (!lua_isinteger(state, index + 1)) {
                    lua_pushnil(state);
                    lua_pushliteral(state, "signed integer argument required");
                    return 2;
                }
                arguments[static_cast<std::size_t>(index)] = static_cast<std::int64_t>(lua_tointeger(state, index + 1));
            }
            const auto result = Invoke(Owner(state), lua_tostring(state, lua_upvalueindex(1)),
                                       std::span{arguments}.first(static_cast<std::size_t>(expected)));
            if (result.success) {
                lua_pushinteger(state, static_cast<lua_Integer>(result.value));
                return 1;
            }
            lua_pushnil(state);
            lua_newtable(state);
            lua_pushstring(state, result.domain.data());
            lua_setfield(state, -2, "domain");
            lua_pushstring(state, result.code.data());
            lua_setfield(state, -2, "code");
            lua_pushstring(state, result.error.data());
            lua_setfield(state, -2, "message");
            return 2;
        }

        void Api(lua_State *state) {
            lua_newtable(state);
            for (const auto &function : Owner(state).binding.Functions()) {
                lua_pushlstring(state, function.id.data(), function.id.size());
                lua_pushinteger(state, static_cast<lua_Integer>(function.parameters.size()));
                lua_pushcclosure(state, Method, 2);
                lua_setfield(state, -2, function.id.c_str());
            }
        }

        int Run(lua_State *state) {
            const auto &execution = Owner(state);
            luaopen_base(state);
            lua_pop(state, 1);
            if (luaL_loadbufferx(state, execution.source.data(), execution.source.size(), "extension-reference", "t") != LUA_OK)
                return lua_error(state);
            lua_newtable(state);
            for (const auto *name : {"assert", "error", "tostring"}) {
                lua_getglobal(state, name);
                lua_setfield(state, -2, name);
            }
            Api(state);
            lua_setfield(state, -2, "api");
            lua_setupvalue(state, -2, 1);
            lua_call(state, 0, 1);
            return 1;
        }

        bool Limits(const LuaExtensionServiceLimits &limits, const std::string_view source) noexcept {
            return limits.maximumSourceBytes > 0 && limits.maximumSourceBytes <= 64U * 1024U &&
                   source.size() <= limits.maximumSourceBytes && limits.maximumMemoryBytes >= 64U * 1024U &&
                   limits.maximumMemoryBytes <= 16U * 1024U * 1024U && limits.maximumInstructions >= HookQuantum &&
                   limits.maximumInstructions <= 1'000'000 && limits.maximumInstructions % HookQuantum == 0;
        }
    }  // namespace

    Result<std::int64_t> ExecuteLuaExtensionService(NativeExtensionScriptSession &binding, const std::string_view source,
                                                    const CancellationToken &cancellation, const LuaExtensionServiceLimits limits) {
        static_assert(sizeof(lua_Integer) == sizeof(std::int64_t));
        if (!Limits(limits, source))
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        Execution execution{binding, cancellation, limits, 0, 0, source};
        auto *state = lua_newstate(Allocate, &execution);
        if (!state)
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::ScriptValueCapacityExceeded));
        *static_cast<Execution **>(lua_getextraspace(state)) = &execution;
        lua_sethook(state, Hook, LUA_MASKCOUNT, HookQuantum);
        lua_pushcfunction(state, Run);
        const auto status = lua_pcall(state, 0, 1, 0);
        const bool valid = status == LUA_OK && lua_isinteger(state, -1);
        const auto value = valid ? static_cast<std::int64_t>(lua_tointeger(state, -1)) : 0;
        lua_close(state);
        if (cancellation.IsCancellationRequested())
            return Result<std::int64_t>::Failure(MakeError(ExtensionErrors::ScriptInvocationCancelled));
        if (!valid)
            return Result<std::int64_t>::Failure(
                MakeError(ExtensionErrors::InvocationFailed, "Lua reference script failed, exceeded its budget or returned a noninteger."));
        return Result<std::int64_t>::Success(value);
    }

    Result<std::int64_t> ExecuteHeadlessLuaExtensionService(HeadlessExtensionHost &host, const std::string_view extension,
                                                            const std::string_view moduleId, const std::string_view importId,
                                                            ScriptCapabilityContextDescriptor policy, const std::string_view source,
                                                            const CancellationToken &cancellation, const LuaExtensionServiceLimits limits) {
        auto binding = host.BindScript(extension, moduleId, importId, std::move(policy), cancellation);
        if (binding.HasError())
            return Result<std::int64_t>::Failure(binding.ErrorValue());
        return ExecuteLuaExtensionService(*binding.Value(), source, cancellation, limits);
    }

}  // namespace Horo::Extensions
