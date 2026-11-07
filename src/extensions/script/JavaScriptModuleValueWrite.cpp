#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleValues.h"

namespace Horo::Extensions::JavaScriptModuleDetail {
    HandleBox::~HandleBox() {
        if (budget && cost) {
            budget->bytes -= cost;
            --budget->values;
        }
    }

    void FinalizeHandle(JSRuntime *, JSValue value) noexcept {
        // The class ID belongs to this engine-owned runtime, never a script-supplied class.
        delete static_cast<HandleBox *>(JS_GetOpaque(value, JS_GetClassID(value)));
    }

    JSValue Values::WriteHandle(const ScriptHandle &handle) {
        auto valid = authority->ValidateHandle(handle);
        if (valid.HasError())
            throw ConversionFailure{valid.ErrorValue()};
        const auto cost = sizeof(HandleBox) + handle.type.capacity() + 1;
        if (handles->values >= handles->maximumValues || cost > handles->maximumBytes - handles->bytes)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptValueCapacityExceeded)};
        Value object{context, Checked(JS_NewObjectClass(context, handleClass))};
        auto box = std::make_unique<HandleBox>();
        box->handle = handle;
        box->budget = handles;
        box->cost = cost;
        ++handles->values;
        handles->bytes += cost;
        if (JS_SetOpaque(object.Get(), box.get()) < 0)
            InvalidValue("Opaque handle installation failed.");
        (void)box.release();
        if (JS_FreezeObject(context, object.Get()) < 0)
            InvalidValue("Opaque handle sealing failed.");
        return object.Release();
    }

    namespace {
        void SetIndex(Values &values, JSValueConst array, std::uint32_t index, JSValue child) {
            if (JS_SetPropertyUint32(values.context, array, index, child) < 0)
                InvalidValue("Array output allocation failed.");
        }

        JSValue WriteArray(Values &values, std::span<const ScriptValue> source, Budget &budget, std::size_t depth) {
            Value array{values.context, Checked(JS_NewArray(values.context))};
            std::uint32_t index{};
            for (const auto &child : source)
                SetIndex(values, array.Get(), index++, values.Write(child, budget, depth + 1));
            return array.Release();
        }

        JSValue WriteMap(Values &values, std::span<const ScriptValue::MapEntry> source, Budget &budget, std::size_t depth) {
            Value array{values.context, Checked(JS_NewArray(values.context))};
            std::uint32_t index{};
            for (const auto &[key, item] : source) {
                Value pair{values.context, Checked(JS_NewArray(values.context))};
                SetIndex(values, pair.Get(), 0, values.Write(key, budget, depth + 1));
                SetIndex(values, pair.Get(), 1, values.Write(item, budget, depth + 1));
                SetIndex(values, array.Get(), index++, pair.Release());
            }
            return array.Release();
        }

        JSValue WriteStruct(Values &values, std::span<const ScriptValue::StructField> source, Budget &budget, std::size_t depth) {
            Value object{values.context, Checked(JS_NewObjectProto(values.context, JS_NULL))};
            for (const auto &[key, item] : source) {
                budget.Charge(depth, key.size());
                auto atom = JS_NewAtomLen(values.context, key.data(), key.size());
                const int status =
                    JS_DefinePropertyValue(values.context, object.Get(), atom, values.Write(item, budget, depth + 1), JS_PROP_C_W_E);
                JS_FreeAtom(values.context, atom);
                if (status < 0)
                    InvalidValue("Struct output allocation failed.");
            }
            return object.Release();
        }

        JSValue WriteBytes(Values &values, std::span<const std::byte> source, Budget &budget) {
            budget.Charge(0, source.size());
            Value array{values.context, Checked(JS_NewArray(values.context))};
            std::uint32_t index{};
            for (auto byte : source)
                SetIndex(values, array.Get(), index++, JS_NewInt32(values.context, std::to_integer<int>(byte)));
            return array.Release();
        }
    }  // namespace

    JSValue Values::WriteCollection(const ScriptValue &value, Budget &budget, std::size_t depth) {
        switch (value.GetKind()) {
            case ScriptValue::Kind::Bytes:
                return WriteBytes(*this, value.AsBytes(), budget);
            case ScriptValue::Kind::Array:
                return WriteArray(*this, value.AsArray(), budget, depth);
            case ScriptValue::Kind::Map:
                return WriteMap(*this, value.AsMap(), budget, depth);
            case ScriptValue::Kind::Struct:
                return WriteStruct(*this, value.AsStruct(), budget, depth);
            default:
                InvalidValue("Unsupported owned value kind.");
        }
    }

    JSValue Values::Write(const ScriptValue &value, Budget &budget, std::size_t depth) {
        budget.Charge(depth);
        using K = ScriptValue::Kind;
        switch (value.GetKind()) {
            case K::Null:
                return JS_NULL;
            case K::Boolean:
                return JS_NewBool(context, *value.AsBoolean());
            case K::SignedInteger:
                return Checked(JS_NewBigInt64(context, *value.AsSignedInteger()));
            case K::UnsignedInteger:
                return Checked(JS_NewBigUint64(context, *value.AsUnsignedInteger()));
            case K::Number:
                return JS_NewFloat64(context, *value.AsNumber());
            case K::String: {
                auto text = value.AsString();
                budget.Charge(depth, text.size());
                return Checked(JS_NewStringLen(context, text.data(), text.size()));
            }
            case K::Handle:
                return WriteHandle(*value.AsHandle());
            case K::Enum:
                return Checked(JS_NewBigInt64(context, value.AsEnum()->value));
            default:
                return WriteCollection(value, budget, depth);
        }
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
