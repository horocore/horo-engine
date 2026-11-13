#pragma once
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "JavaScriptModuleValues.h"
#include "ScriptModuleProjection.h"
#include "ScriptModuleRebindInternal.h"

#include <array>
#include <atomic>
#include <thread>

namespace Horo::Extensions::JavaScriptModuleDetail {
    struct Pending final {
        ScriptInvocationHandle handle;
        JSValue resolve{JS_UNDEFINED};
        JSValue reject{JS_UNDEFINED};
        std::shared_ptr<ScriptModuleContext> origin;
    };

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
        JSRuntime *runtime{};
        JSContext *context{};
        Values values;
        JSValue program{JS_UNDEFINED};
        JSValue completion{JS_UNDEFINED};
        JSValue horo{JS_UNDEFINED};
        std::vector<JSValue> imports;
        std::vector<Pending> pending;
        std::optional<ScriptCallResult> result;
        std::chrono::steady_clock::time_point deadline{};
        bool started{};
        bool executing{};
        ScriptCallResult emergency{MakeScriptModuleEmergencyResult()};

        explicit State(std::shared_ptr<ScriptModuleContext> value)
            : authority(std::move(value)), published(std::make_shared<const ScriptModulePublishedContexts>(authority, nullptr)),
              activeOnly(published) {}

        ~State();
        bool Live() const noexcept;
        void RequireOwner() const;
        void RequireIdle() const;
        void RequireLive() const;
        void Initialize();
        void Intrinsics();
        void Classes();
        void BindImports();
        [[nodiscard]] JSValue Call(std::string_view, std::string_view, int, JSValueConst *);
        [[nodiscard]] JSValue Async(ScriptInvocationHandle);
        [[nodiscard]] JSValue ImmediateAsync(ScriptCallResult);
        void Prepare(std::string_view);
        void Start();
        void Poll();
        void Deliver();
        void Jobs();
        void Observe();
        void Cancel() noexcept;
        [[nodiscard]] bool PendingFrom(const std::shared_ptr<ScriptModuleContext> &) const noexcept;
        [[nodiscard]] bool PollPredecessor();
    };

    [[nodiscard]] std::vector<JSValue> PrepareJavaScriptNamespaces(State &, const std::shared_ptr<ScriptModuleContext> &,
                                                                   const std::vector<std::shared_ptr<const ScriptModuleBindingPlan>> &);
    [[nodiscard]] Result<std::unique_ptr<ScriptModulePreparedRebind>> PrepareJavaScriptRebind(std::shared_ptr<State>,
                                                                                              std::shared_ptr<ScriptModuleContext>);

    struct Active final {
        State &state;

        explicit Active(State &value) : state(value) {
            state.RequireLive();
            state.executing = true;
        }

        Active(const Active &) = delete;
        Active &operator=(const Active &) = delete;

        ~Active() {
            state.executing = false;
        }
    };

    [[nodiscard]] JSValue Import(JSContext *, JSValueConst, int, JSValueConst *);
    [[nodiscard]] JSValue Invoke(JSContext *, JSValueConst, int, JSValueConst *, int, JSValueConst *);
    [[nodiscard]] JSValue Failure(JSContext *) noexcept;
    [[nodiscard]] JSValue StructuredFailure(JSContext *, const Error &) noexcept;
}  // namespace Horo::Extensions::JavaScriptModuleDetail
