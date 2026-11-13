#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"
#include "JavaScriptModuleInternal.h"

namespace Horo::Extensions::JavaScriptModuleDetail {
    namespace {
        ScriptCallResult ErrorResult(const Error &error, const ScriptValueLimits &limits) {
            auto converted = MakeScriptError(error, false, limits);
            if (converted.HasError())
                throw ConversionFailure{converted.ErrorValue()};
            return ScriptCallResult::Failure(std::move(converted).Value());
        }

        void IgnoreException(JSContext *context) {
            Value discarded{context, JS_GetException(context)};
        }
    }  // namespace

    JSValue State::ImmediateAsync(ScriptCallResult result) {
        JSValue callbacks[]{JS_UNDEFINED, JS_UNDEFINED};
        Value promise{context, Checked(JS_NewPromiseCapability(context, callbacks))};
        Value resolve{context, callbacks[0]};
        Value reject{context, callbacks[1]};
        Value outcome{context, values.WriteOutcome(std::move(result))};
        auto argument = outcome.Get();
        Value settled{context, Checked(JS_Call(context, resolve.Get(), JS_UNDEFINED, 1, &argument))};
        return promise.Release();
    }

    JSValue State::Async(ScriptInvocationHandle handle) {
        if (pending.size() >= authority->Limits().maximumPendingCalls)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded)};
        JSValue callbacks[]{JS_UNDEFINED, JS_UNDEFINED};
        Value promise{context, Checked(JS_NewPromiseCapability(context, callbacks))};
        Value resolve{context, callbacks[0]};
        Value reject{context, callbacks[1]};
        pending.push_back({std::move(handle), resolve.Get(), reject.Get(), authority});
        (void)resolve.Release();
        (void)reject.Release();
        return promise.Release();
    }

    JSValue State::Call(std::string_view imported, std::string_view method, int argc, JSValueConst *argv) {
        RequireLive();
        if (argc < 0)
            InvalidValue("Negative argument count.");
        auto selected = FindScriptModuleFunction(authority->Imports(), imported, method, static_cast<std::size_t>(argc));
        if (selected.HasError())
            throw ConversionFailure{selected.ErrorValue()};
        const auto &api = *selected.Value().api;
        const auto &function = *selected.Value().function;
        Budget budget{authority->Limits().values, &deadline};
        std::vector<ScriptValue> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i)
            arguments.push_back(values.Read(argv[i], function.parameters[static_cast<std::size_t>(i)].type, api, budget));
        auto call = authority->Invoke(selected.Value().imported->Alias(), function.id, std::move(arguments));
        if (call.HasError()) {
            auto result = ErrorResult(call.ErrorValue(), authority->Limits().values);
            return function.invocation == ScriptExportInvocationMode::Asynchronous ? ImmediateAsync(std::move(result))
                                                                                   : values.WriteOutcome(std::move(result));
        }
        if (function.invocation == ScriptExportInvocationMode::Asynchronous)
            return Async(std::move(call).Value());
        auto snapshot = call.Value().Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            InvalidValue("Synchronous invocation did not produce its actual terminal outcome.");
        // Drain the actual canonical event queue, including prior async calls; handles retain exact immutable results.
        if (auto drained = authority->Poll(); drained.HasError())
            throw ConversionFailure{drained.ErrorValue()};
        return values.WriteOutcome(std::move(*snapshot->terminalResult));
    }

    void State::Prepare(std::string_view source) {
        RequireIdle();
        RequireLive();
        if (source.size() > authority->Limits().maximumSourceBytes)
            InvalidValue("Module source exceeds its byte limit.");
        if (source.find('\0') != std::string_view::npos || !IsValidUtf8ScalarSequence(source))
            InvalidValue("Module source must be complete UTF-8 text without embedded NUL.");
        if (started || source.size() > authority->Limits().maximumSourceBytes)
            InvalidValue("Module source is too large or has already started.");
        sourceBytes = source.size();
        sourceDigest = ComputeSha256(std::as_bytes(std::span{source.data(), source.size()}));
        deadline = std::chrono::steady_clock::now() + authority->Limits().executionDeadline;
        Active active{*this};
        std::string wrapped = "(async function(horo){\"use strict\";\n";
        wrapped += source;
        wrapped += "\n})";
        program = Checked(
            JS_Eval(context, wrapped.c_str(), wrapped.size(), "horo/module-operation.js", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY));
    }

    void State::Start() {
        RequireLive();
        Active active{*this};
        started = true;
        Value function{context, JS_EvalFunction(context, program)};
        program = JS_UNDEFINED;
        if (JS_IsException(function.Get())) {
            IgnoreException(context);
            result = ErrorResult(MakeError(ExtensionErrors::ScriptInvocationInvalid, "JavaScript program initialization failed."),
                                 authority->Limits().values);
            return;
        }
        completion = JS_Call(context, function.Get(), JS_UNDEFINED, 1, &horo);
        if (JS_IsException(completion)) {
            completion = JS_UNDEFINED;
            IgnoreException(context);
            result = ErrorResult(MakeError(ExtensionErrors::ScriptInvocationInvalid, "JavaScript program execution failed."),
                                 authority->Limits().values);
        } else
            Observe();
    }

    void State::Deliver() {
        for (auto item = pending.begin(); item != pending.end();) {
            auto snapshot = item->handle.Snapshot();
            if (!snapshot || !snapshot->terminalResult) {
                ++item;
                continue;
            }
            auto projector = values;
            projector.authority = item->origin;
            Value outcome{context, projector.WriteOutcome(std::move(*snapshot->terminalResult))};
            auto argument = outcome.Get();
            Value delivered{context, Checked(JS_Call(context, item->resolve, JS_UNDEFINED, 1, &argument))};
            JS_FreeValue(context, item->resolve);
            JS_FreeValue(context, item->reject);
            item = pending.erase(item);
        }
    }

    void State::Jobs() {
        for (std::size_t index = 0; index < authority->Limits().maximumVmJobsPerPoll; ++index) {
            RequireLive();
            JSContext *jobContext{};
            const int status = JS_ExecutePendingJob(runtime, &jobContext);
            if (status < 0) {
                IgnoreException(jobContext ? jobContext : context);
                InvalidValue("A JavaScript promise job failed.");
            }
            if (status == 0)
                break;
        }
    }

    void State::Observe() {
        if (result || JS_IsUndefined(completion))
            return;
        const auto status = JS_PromiseState(context, completion);
        if (status == JS_PROMISE_PENDING)
            return;
        Value value{context, JS_PromiseResult(context, completion)};
        if (status != JS_PROMISE_FULFILLED) {
            if (!JS_IsProxy(value.Get()) && JS_GetClassID(value.Get()) == values.outcomeClass) {
                result = values.ReadOutcome(value.Get());
                return;
            }
            result = ErrorResult(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                           "JavaScript module rejected; error objects are not coerced or evaluated."),
                                 authority->Limits().values);
            return;
        }

        result = values.ReadOutcome(value.Get());
    }

    void State::Poll() {
        RequireIdle();
        static_cast<void>(PollPredecessor());
        RequireLive();
        if (!started)
            InvalidValue("Module program has not started.");
        if (result)
            return;
        Active active{*this};
        if (auto events = authority->Poll(); events.HasError())
            throw ConversionFailure{events.ErrorValue()};
        Deliver();
        Jobs();
        Observe();
        static_cast<void>(PollPredecessor());
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
