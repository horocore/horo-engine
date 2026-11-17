#pragma once
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "LuaModuleValues.h"
#include "ScriptModuleProjection.h"
#include "ScriptModuleRebindInternal.h"

#include <array>
#include <atomic>
#include <thread>

namespace Horo::Extensions::LuaModuleDetail {
    struct State final {
        std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<ScriptModuleContext> authority;
        std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans;
        std::shared_ptr<ScriptModuleContext> predecessor;
        // Only this immutable actual-context bundle crosses lanes for Close; owner VM fields are never worker-readable.
        std::shared_ptr<const ScriptModulePublishedContexts> published;
        std::shared_ptr<const ScriptModulePublishedContexts> activeOnly;
        std::array<std::shared_ptr<const ScriptModulePublishedContexts>, 2> retiredPublications;
        std::uint64_t bindingRevision{1};
        bool rebindPrepared{};
        std::optional<Error> predecessorDiagnostic;
        Sha256Digest sourceDigest;
        std::size_t sourceBytes{};
        std::atomic<bool> closed{};
        lua_State *lua{};
        lua_State *thread{};
        int threadRef{LUA_NOREF};
        int horoRef{LUA_NOREF};
        int importsRef{LUA_NOREF};
        Values values;
        std::size_t allocated{};
        std::uint32_t instructions{};
        std::chrono::steady_clock::time_point deadline{};
        bool prepared{};
        bool started{};
        bool awaiting{};
        bool executing{};
        ScriptInvocationHandle pending;
        std::shared_ptr<ScriptModuleContext> pendingOrigin;
        std::optional<ScriptCallResult> result;
        std::optional<ScriptCallResult> scratchResult;
        std::optional<ScriptValue> scratchValue;
        std::optional<Error> bridgeError;
        ScriptCallResult emergency{MakeScriptModuleEmergencyResult()};

        explicit State(std::shared_ptr<ScriptModuleContext> context)
            : authority(std::move(context)), published(std::make_shared<const ScriptModulePublishedContexts>(authority, nullptr)),
              activeOnly(published) {}

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
        bool PrepareCall(lua_State *, std::string_view, std::string_view) noexcept;
        bool PrepareIndex(lua_State *) noexcept;
        void PrepareUnsignedText();
        bool ProjectOutcome(const Box &, std::string_view);
        bool PrepareUnsigned(lua_State *) noexcept;
        bool RecordError(const Error &) noexcept;
        bool PrepareConstant(const ScriptExportConstantDescriptor &) noexcept;
        void CaptureOutcome();
        [[nodiscard]] bool PendingFrom(const std::shared_ptr<ScriptModuleContext> &) const noexcept;
        [[nodiscard]] bool PollPredecessor();
    };

    [[nodiscard]] int PrepareLuaNamespaces(State &, const std::shared_ptr<ScriptModuleContext> &,
                                           const std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> &);
    [[nodiscard]] Result<std::unique_ptr<ScriptModulePreparedRebind>> PrepareLuaRebind(std::shared_ptr<State>,
                                                                                       std::shared_ptr<ScriptModuleContext>);
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
