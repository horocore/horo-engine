#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"
#include "LuaModuleInternal.h"

namespace Horo::Extensions::LuaModuleDetail {
    namespace {
        ScriptCallResult ErrorResult(const Error &error, const ScriptValueLimits &limits) {
            auto converted = MakeScriptError(error, false, limits);
            if (converted.HasError())
                throw Failure{converted.ErrorValue()};
            return ScriptCallResult::Failure(std::move(converted).Value());
        }

        Error ExecutionError(const State &state) {
            if (!state.Live())
                return MakeError(ExtensionErrors::ScriptInvocationContextRevoked);
            if (std::chrono::steady_clock::now() >= state.deadline)
                return MakeError(ExtensionErrors::ScriptInvocationTimeout);
            return MakeError(ExtensionErrors::ScriptInvocationInvalid,
                             "Lua module execution failed its declared runtime or instruction bound.");
        }
    }  // namespace

    void State::Prepare(std::string_view source) {
        RequireIdle();
        RequireLive();
        if (source.size() > authority->Limits().maximumSourceBytes)
            Invalid("Module source exceeds its byte limit.");
        if (source.find('\0') != std::string_view::npos || !IsValidUtf8ScalarSequence(source))
            Invalid("Module source must be complete UTF-8 text without embedded NUL.");
        if (prepared || source.size() > authority->Limits().maximumSourceBytes)
            Invalid("Module source is oversized or already prepared.");
        sourceBytes = source.size();
        sourceDigest = ComputeSha256(std::as_bytes(std::span{source.data(), source.size()}));
        deadline = std::chrono::steady_clock::now() + authority->Limits().executionDeadline;
        const std::string wrapped = std::string{"local horo=...\n"} + std::string{source};
        if (luaL_loadbufferx(thread, wrapped.data(), wrapped.size(), "horo/module-operation.lua", "t") != LUA_OK)
            Invalid("Module source is not a valid text-only Lua program.");
        prepared = true;
    }

    void State::Resume(int count) {
        int returned{};

        struct Active final {
            bool &flag;

            explicit Active(bool &value) : flag(value) {
                flag = true;
            }

            ~Active() {
                flag = false;
            }
        } active{executing};

        const int status = lua_resume(thread, lua, count, &returned);
        values.lua = thread;
        if (status == LUA_YIELD) {
            if (!awaiting || !pending.IsValid())
                Invalid("Only a canonical asynchronous host invocation may suspend the module.");
            return;
        }
        if (status != LUA_OK) {
            result = ErrorResult(ExecutionError(*this), authority->Limits().values);
            return;
        }
        if (returned != 1)
            Invalid("A module program must return exactly one host call outcome.");
        CaptureOutcome();
    }

    void State::Start() {
        RequireLive();
        if (!prepared || started)
            Invalid("Module program is not newly prepared.");
        started = true;
        if (!lua_checkstack(thread, static_cast<int>(authority->Limits().values.maximumDepth * 4 + 32)))
            Invalid("Module coroutine stack admission failed.");
        lua_rawgeti(lua, LUA_REGISTRYINDEX, horoRef);
        lua_xmove(lua, thread, 1);
        Resume(1);
    }

    void State::Poll() {
        RequireIdle();
        static_cast<void>(PollPredecessor());
        RequireLive();
        if (!started)
            Invalid("Module program has not started.");
        if (result)
            return;
        auto events = authority->Poll();
        if (events.HasError())
            throw Failure{events.ErrorValue()};
        if (!awaiting)
            return;
        auto snapshot = pending.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return;
        scratchResult = std::move(*snapshot->terminalResult);
        values.PrepareOutcome(*scratchResult);
        scratchResult.reset();
        if (!lua_checkstack(lua, static_cast<int>(authority->Limits().values.maximumDepth * 4 + 32)))
            Invalid("Owner output stack admission failed.");
        lua_pushcfunction(lua, PushOutcomeValues);
        if (lua_pcall(lua, 0, 1, 0) != LUA_OK)
            Invalid("Asynchronous outcome projection exceeded its declared bound.");
        lua_xmove(lua, thread, 1);
        pending = {};
        pendingOrigin.reset();
        awaiting = false;
        Resume(1);
        static_cast<void>(PollPredecessor());
    }
}  // namespace Horo::Extensions::LuaModuleDetail
