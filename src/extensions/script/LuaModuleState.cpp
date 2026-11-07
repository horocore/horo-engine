#include "Horo/Extensions/ExtensionErrors.h"
#include "LuaModuleInternal.h"

#include <cassert>
#include <cstdlib>
#include <limits>

namespace Horo::Extensions::LuaModuleDetail {
    namespace {
        struct Allocation final {
            std::size_t bytes;
        };

        void Remove(lua_State *lua, const char *name) {
            lua_pushnil(lua);
            lua_setglobal(lua, name);
        }

        void Open(lua_State *lua, const char *name, lua_CFunction function) {
            luaL_requiref(lua, name, function, 1);
            lua_pop(lua, 1);
        }
    }  // namespace

    void *Allocate(void *opaque, void *pointer, std::size_t, std::size_t size) noexcept {
        auto &state = *static_cast<State *>(opaque);
        auto *old = pointer ? static_cast<Allocation *>(pointer) - 1 : nullptr;
        const auto previous = old ? old->bytes : 0;
        if (size == 0) {
            state.allocated -= previous;
            std::free(old);
            return nullptr;
        }
        if (size > std::numeric_limits<std::size_t>::max() - sizeof(Allocation))
            return nullptr;
        const auto cost = size + sizeof(Allocation);
        if (cost > state.authority->Limits().maximumMemoryBytes - (state.allocated - previous))
            return nullptr;
        auto *replacement = static_cast<Allocation *>(std::realloc(old, cost));
        if (!replacement)
            return nullptr;
        replacement->bytes = cost;
        state.allocated = state.allocated - previous + cost;
        return replacement + 1;
    }

    State &Owner(lua_State *lua) {
        return **static_cast<State **>(lua_getextraspace(lua));
    }

    void Hook(lua_State *lua, lua_Debug *) {
        auto &state = Owner(lua);
        if (!state.Live())
            luaL_error(lua, "Module authority has been revoked.");
        if (std::chrono::steady_clock::now() >= state.deadline)
            luaL_error(lua, "Module execution deadline exceeded.");
        if (state.instructions <= 1000)
            luaL_error(lua, "Module instruction ceiling reached.");
        state.instructions -= 1000;
    }

    bool State::Live() const noexcept {
        return !closed.load(std::memory_order_acquire) && authority->IsUsable();
    }

    void State::RequireOwner() const {
        if (owner != std::this_thread::get_id())
            throw Failure{MakeError(ExtensionErrors::ScriptInvocationThreadViolation)};
    }

    void State::RequireIdle() const {
        RequireOwner();
        if (executing)
            throw Failure{MakeError(ExtensionErrors::ScriptInvocationReentrant)};
    }

    void State::RequireLive() const {
        RequireOwner();
        if (!Live())
            throw Failure{MakeError(ExtensionErrors::ScriptInvocationContextRevoked)};
        if (started && std::chrono::steady_clock::now() >= deadline)
            throw Failure{MakeError(ExtensionErrors::ScriptInvocationTimeout)};
    }

    int Setup(lua_State *lua) {
        auto &state = Owner(lua);
        Open(lua, "_G", luaopen_base);
        Open(lua, LUA_TABLIBNAME, luaopen_table);
        Open(lua, LUA_STRLIBNAME, luaopen_string);
        Open(lua, LUA_MATHLIBNAME, luaopen_math);
        Open(lua, LUA_UTF8LIBNAME, luaopen_utf8);
        // No protected user calls: script must not catch and repeatedly suppress the host's budget hook.
        constexpr const char *removed[]{"dofile", "loadfile", "load",   "collectgarbage", "print",
                                        "warn",   "pcall",    "xpcall", "setmetatable",   "getmetatable"};
        for (auto name : removed)
            Remove(lua, name);
        lua_getglobal(lua, "string");
        lua_pushnil(lua);
        lua_setfield(lua, -2, "dump");
        lua_pop(lua, 1);
        luaL_newmetatable(lua, "horo.module.value");
        lua_pushcfunction(lua, CollectBox);
        lua_setfield(lua, -2, "__gc");
        lua_pushcfunction(lua, BoxName);
        lua_setfield(lua, -2, "__tostring");
        lua_pushcfunction(lua, IndexBox);
        lua_setfield(lua, -2, "__index");
        lua_pushcfunction(lua, EqualBox);
        lua_setfield(lua, -2, "__eq");
        lua_pushboolean(lua, 0);
        lua_setfield(lua, -2, "__metatable");
        state.values.metatable = luaL_ref(lua, LUA_REGISTRYINDEX);
        lua_newtable(lua);
        lua_pushcfunction(lua, Import);
        lua_setfield(lua, -2, "import");
        lua_pushcfunction(lua, Unsigned);
        lua_setfield(lua, -2, "u64");
        state.horoRef = luaL_ref(lua, LUA_REGISTRYINDEX);
        state.thread = lua_newthread(lua);
        state.threadRef = luaL_ref(lua, LUA_REGISTRYINDEX);
        return 0;
    }

    void State::Initialize() {
        plans.reserve(authority->Imports().size());
        for (const auto &imported : authority->Imports()) {
            if (!imported.Backend()) {
                plans.push_back({});
                continue;
            }
            auto plan = authority->BindingPlan(imported.Alias());
            if (plan.HasError())
                throw Failure{plan.ErrorValue()};
            plans.push_back(std::move(plan).Value());
        }

        static_assert(sizeof(lua_Integer) == sizeof(std::int64_t));
        deadline = std::chrono::steady_clock::now() + authority->Limits().executionDeadline;
        instructions = authority->Limits().luaInstructionBudget;
        lua = lua_newstate(Allocate, this);
        if (!lua)
            Invalid("Lua allocation failed.");
        *static_cast<State **>(lua_getextraspace(lua)) = this;
        lua_pushcfunction(lua, Setup);
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK)
            Invalid("Lua module profile initialization failed.");
        values.lua = thread;
        values.authority = authority;
        values.deadline = &deadline;
        values.ledger = std::make_shared<Ledger>();
        values.ledger->maximumBytes = authority->Limits().maximumBridgeBytes;
        values.ledger->maximumValues = authority->Limits().maximumBridgeValues;
        lua_pushcfunction(lua, BindImports);
        if (lua_pcall(lua, 0, 0, 0) != LUA_OK)
            Invalid("Lua import projection failed.");
        lua_sethook(thread, Hook, LUA_MASKCOUNT, 1000);
    }

    void State::Cancel() noexcept {
        closed.store(true, std::memory_order_seq_cst);
        const auto actual = std::atomic_load_explicit(&published, std::memory_order_seq_cst);
        if (actual)
            actual->Close();
    }

    State::~State() {
        assert(owner == std::this_thread::get_id());
        Cancel();
        if (lua)
            lua_close(lua);
    }
}  // namespace Horo::Extensions::LuaModuleDetail
