#pragma once
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "JavaScriptModuleValues.h"
#include "ScriptModuleProjection.h"

#include <atomic>
#include <thread>

namespace Horo::Extensions::JavaScriptModuleDetail {
    struct Pending final {
        ScriptInvocationHandle handle;
        JSValue resolve{JS_UNDEFINED};
        JSValue reject{JS_UNDEFINED};
    };

    struct State final {
        std::thread::id owner{std::this_thread::get_id()};
        std::shared_ptr<ScriptModuleContext> authority;
        std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> plans;
        std::atomic<bool> closed{};
        JSRuntime *runtime{};
        JSContext *context{};
        Values values;
        JSValue program{JS_UNDEFINED}, completion{JS_UNDEFINED}, horo{JS_UNDEFINED};
        std::vector<JSValue> imports;
        std::vector<Pending> pending;
        std::optional<ScriptCallResult> result;
        std::chrono::steady_clock::time_point deadline{};
        bool started{}, executing{};
        ScriptCallResult emergency{MakeScriptModuleEmergencyResult()};

        explicit State(std::shared_ptr<ScriptModuleContext> value) : authority(std::move(value)) {}

        ~State();
        bool Live() const noexcept;
        void RequireOwner() const;
        void RequireIdle() const;
        void RequireLive() const;
        void Initialize();
        void Intrinsics();
        void Classes();
        void BindImports();
        [[nodiscard]] JSValue Namespace(std::size_t);
        [[nodiscard]] JSValue Call(std::size_t, std::size_t, int, JSValueConst *);
        [[nodiscard]] JSValue Async(ScriptInvocationHandle);
        [[nodiscard]] JSValue ImmediateAsync(ScriptCallResult);
        void Prepare(std::string_view);
        void Start();
        void Poll();
        void Deliver();
        void Jobs();
        void Observe();
        void Cancel() noexcept;
    };

    struct Active final {
        State &state;

        explicit Active(State &value) : state(value) {
            state.RequireLive();
            state.executing = true;
        }

        ~Active() {
            state.executing = false;
        }
    };

    [[nodiscard]] JSValue Import(JSContext *, JSValueConst, int, JSValueConst *);
    [[nodiscard]] JSValue Invoke(JSContext *, JSValueConst, int, JSValueConst *, int, JSValueConst *);
    [[nodiscard]] JSValue Failure(JSContext *) noexcept;
    [[nodiscard]] JSValue StructuredFailure(JSContext *, const Error &) noexcept;
}  // namespace Horo::Extensions::JavaScriptModuleDetail
