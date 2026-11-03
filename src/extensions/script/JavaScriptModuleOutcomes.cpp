#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleValues.h"

namespace Horo::Extensions::JavaScriptModuleDetail {
    OutcomeBox::~OutcomeBox() {
        if (budget && cost) {
            budget->bytes -= cost;
            --budget->values;
        }
    }

    void FinalizeOutcome(JSRuntime *, JSValue value) noexcept {
        delete static_cast<OutcomeBox *>(JS_GetOpaque(value, JS_GetClassID(value)));
    }

    namespace {
        ScriptValue ErrorValue(const ScriptError &error) {
            ScriptValue::MapEntries details;
            details.reserve(error.details.size());
            for (const auto &detail : error.details)
                details.emplace_back(ScriptValue{detail.id}, detail.value);
            return ScriptValue::Struct("horo.script.error", {{"domain", ScriptValue{error.domain}},
                                                             {"code", ScriptValue{error.code}},
                                                             {"message", ScriptValue{error.message}},
                                                             {"retryable", ScriptValue{error.retryable}},
                                                             {"cancelled", ScriptValue{error.cancelled}},
                                                             {"details", ScriptValue::Map(std::move(details))}});
        }

        void Data(JSContext *context, JSValueConst object, const char *name, JSValue value) {
            if (JS_DefinePropertyValueStr(context, object, name, value, JS_PROP_ENUMERABLE) < 0)
                InvalidValue("Outcome projection allocation failed.");
        }
    }  // namespace

    JSValue Values::WriteOutcome(ScriptCallResult result) {
        auto encoded = EncodeScriptCallResult(result, authority->Limits().values);
        if (encoded.HasError())
            throw ConversionFailure{encoded.ErrorValue()};
        const auto cost = sizeof(OutcomeBox) + encoded.Value().capacity();
        if (handles->values >= handles->maximumValues || cost > handles->maximumBytes - handles->bytes)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptValueCapacityExceeded)};
        Value object{context, Checked(JS_NewObjectClass(context, outcomeClass))};
        auto box = std::make_unique<OutcomeBox>();
        box->encoded = std::move(encoded).Value();
        box->budget = handles;
        box->cost = cost;
        ++handles->values;
        handles->bytes += cost;
        if (JS_SetOpaque(object.Get(), box.get()) < 0)
            InvalidValue("Outcome installation failed.");
        (void)box.release();
        Budget budget{authority->Limits().values, deadline};
        Data(context, object.Get(), "values", Write(ScriptValue::Array(result.values), budget));
        Data(context, object.Get(), "error", result.error ? Write(ErrorValue(*result.error), budget) : JS_NULL);
        if (JS_FreezeObject(context, object.Get()) < 0)
            InvalidValue("Outcome sealing failed.");
        return object.Release();
    }

    ScriptCallResult Values::ReadOutcome(JSValueConst object) const {
        if (JS_IsProxy(object) || JS_GetClassID(object) != outcomeClass)
            InvalidValue("A module program must return an actual host call outcome.");
        auto *box = static_cast<const OutcomeBox *>(JS_GetOpaque(object, outcomeClass));
        if (!box)
            InvalidValue("Outcome has been retired.");
        auto decoded = DecodeScriptCallResult(box->encoded, authority->Limits().values);
        if (decoded.HasError())
            throw ConversionFailure{decoded.ErrorValue()};
        return std::move(decoded).Value();
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
