#pragma once
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "LuaModuleValues.h"
#include "ScriptModuleProjection.h"

#include <atomic>
#include <thread>

namespace Horo::Extensions::LuaModuleDetail {
    struct State final {
        std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<ScriptModuleContext> authority;
        std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans;
        std::atomic<bool> closed{};
        lua_State *lua{}, *thread{};
        int threadRef{LUA_NOREF}, horoRef{LUA_NOREF}, importsRef{LUA_NOREF};
        Values values;
        std::size_t allocated{};
        std::uint32_t instructions{};
        std::chrono::steady_clock::time_point deadline{};
        bool prepared{}, started{}, awaiting{}, executing{};
        ScriptInvocationHandle pending;
        std::optional<ScriptCallResult> result, scratchResult;
        std::optional<ScriptValue> scratchValue;
        std::optional<Error> bridgeError;
        ScriptCallResult emergency{MakeScriptModuleEmergencyResult()};

        explicit State(std::shared_ptr<ScriptModuleContext> context) : authority(std::move(context)) {}

        ~State();
        bool Live() const noexcept;
        void RequireOwner() const;
        void RequireIdle() const;
        void RequireLive() const;
        void Initialize();
        void Prepare(std::string_view);
        void Start();
        void Poll();
        void Resume(int);
        void Cancel() noexcept;
        bool PrepareCall(lua_State *, std::size_t, std::size_t) noexcept;
        bool PrepareIndex(lua_State *) noexcept;
        void PrepareUnsignedText();
        bool ProjectOutcome(const Box &, std::string_view);
        bool PrepareUnsigned(lua_State *) noexcept;
        bool RecordError(const Error &) noexcept;
        bool PrepareConstant(const ScriptExportConstantDescriptor &) noexcept;
        void CaptureOutcome();
    };

    State &Owner(lua_State *);
    int Setup(lua_State *);
    int BindImports(lua_State *);
    int Import(lua_State *);
    int Invoke(lua_State *);
    int Continue(lua_State *, int, lua_KContext);
    int IndexBox(lua_State *);
    int EqualBox(lua_State *);
    int Unsigned(lua_State *);
    void Hook(lua_State *, lua_Debug *);
    void *Allocate(void *, void *, std::size_t, std::size_t) noexcept;
    int PushOutcomeValues(lua_State *);
}  // namespace Horo::Extensions::LuaModuleDetail
