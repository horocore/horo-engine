#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleInternal.h"

namespace Horo::Extensions::JavaScriptModuleDetail {
    namespace {
        int Interrupt(JSContext *, void *opaque) {
            const auto &state = *static_cast<const State *>(opaque);
            return !state.Live() || (state.executing && std::chrono::steady_clock::now() >= state.deadline);
        }

        void Add(int status) {
            if (status < 0)
                InvalidValue("Required JavaScript intrinsic initialization failed.");
        }

        void Class(State &state, JSClassID &id, const char *name, JSClassFinalizer *finalizer) {
            JS_NewClassID(state.runtime, &id);
            JSClassDef definition{};
            definition.class_name = name;
            definition.finalizer = finalizer;
            Add(JS_NewClass(state.runtime, id, &definition));
            JS_SetClassProto(state.context, id, JS_NULL);
        }
    }  // namespace

    bool State::Live() const noexcept {
        return !closed.load(std::memory_order_acquire) && authority->IsUsable();
    }

    void State::RequireOwner() const {
        if (owner != std::this_thread::get_id())
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationThreadViolation)};
    }

    void State::RequireIdle() const {
        RequireOwner();
        if (executing)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationReentrant)};
    }

    void State::RequireLive() const {
        RequireOwner();
        if (!Live())
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationContextRevoked)};
        if (started && std::chrono::steady_clock::now() >= deadline)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationTimeout)};
    }

    void State::Intrinsics() {
        Add(JS_AddIntrinsicBaseObjects(context));
        Add(JS_AddIntrinsicEval(context));
        Add(JS_AddIntrinsicPromise(context));
        Add(JS_AddIntrinsicBigInt(context));
        // No TypedArrays/SharedArrayBuffer/Atomics, workers, standard library or host loaders.
        Value object{context, Checked(JS_NewObject(context))};
        values.objectClass = JS_GetClassID(object.Get());
        Value array{context, Checked(JS_NewArray(context))};
        values.arrayClass = JS_GetClassID(array.Get());
    }

    void State::Classes() {
        values.context = context;
        values.authority = authority;
        values.deadline = &deadline;
        values.handles = std::make_shared<HandleBudget>();
        values.handles->maximumBytes = authority->Limits().maximumBridgeBytes;
        values.handles->maximumValues = authority->Limits().maximumBridgeValues;
        Class(*this, values.handleClass, "HoroModuleHandle", FinalizeHandle);
        Class(*this, values.outcomeClass, "HoroModuleOutcome", FinalizeOutcome);
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
                throw ConversionFailure{plan.ErrorValue()};
            plans.push_back(std::move(plan).Value());
        }

        deadline = std::chrono::steady_clock::now() + authority->Limits().executionDeadline;
        runtime = JS_NewRuntime();
        if (!runtime)
            InvalidValue("JavaScript runtime allocation failed.");
        JS_SetMemoryLimit(runtime, authority->Limits().maximumMemoryBytes);
        JS_SetMaxStackSize(runtime, authority->Limits().maximumStackBytes);
        JS_SetCanBlock(runtime, false);
        JS_SetInterruptHandler(runtime, Interrupt, this);
        context = JS_NewContextRaw(runtime);
        if (!context)
            InvalidValue("JavaScript context allocation failed.");
        JS_SetContextOpaque(context, this);
        Intrinsics();
        Classes();
        BindImports();
        pending.reserve(authority->Limits().maximumPendingCalls);
    }

    void State::Cancel() noexcept {
        closed.store(true, std::memory_order_seq_cst);
        const auto actual = std::atomic_load_explicit(&published, std::memory_order_seq_cst);
        if (actual)
            actual->Close();
        // Canonical context reset requests cancellation without traversing mutable VM state off-lane.
    }

    State::~State() {
        Cancel();
        for (auto &entry : pending) {
            JS_FreeValue(context, entry.resolve);
            JS_FreeValue(context, entry.reject);
        }
        for (auto value : imports)
            JS_FreeValue(context, value);
        if (context) {
            JS_FreeValue(context, program);
            JS_FreeValue(context, completion);
            JS_FreeValue(context, horo);
            JS_FreeContext(context);
        }
        if (runtime)
            JS_FreeRuntime(runtime);
    }

    JSValue StructuredFailure(JSContext *context, const Error &error) noexcept {
        try {
            Value discarded{context, JS_GetException(context)};
            auto *state = static_cast<State *>(JS_GetContextOpaque(context));
            if (!state)
                return Failure(context);
            auto converted = MakeScriptError(error, false, state->authority->Limits().values);
            if (converted.HasError())
                return Failure(context);
            return state->values.WriteOutcome(ScriptCallResult::Failure(std::move(converted).Value()));
        } catch (...) {
            return Failure(context);
        }
    }

    JSValue Failure(JSContext *context) noexcept {
        return JS_ThrowInternalError(context, "Module operation failed its declared type, authority, or execution bound.");
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
