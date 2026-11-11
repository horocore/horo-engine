#include "JavaScriptModuleValues.h"

#include <algorithm>
#include <cmath>

namespace Horo::Extensions::JavaScriptModuleDetail {
    namespace {
        Value Index(Values &values, JSValueConst object, std::uint32_t index) {
            auto atom = JS_NewAtomUInt32(values.context, index);
            auto result = values.Own(object, atom);
            JS_FreeAtom(values.context, atom);
            return result;
        }

        Value Field(Values &values, JSValueConst object, const std::string &name) {
            auto atom = JS_NewAtomLen(values.context, name.data(), name.size());
            auto result = values.Own(object, atom);
            JS_FreeAtom(values.context, atom);
            return result;
        }

        ScriptValue ReadBytes(Values &values, JSValueConst object, Budget &budget) {
            const auto count = values.Length(object, budget);
            budget.Charge(0, count);
            ScriptValue::Bytes bytes;
            bytes.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                auto child = Index(values, object, static_cast<std::uint32_t>(i));
                double byte{};
                if (!JS_IsNumber(child.Get()) || JS_ToFloat64(values.context, &byte, child.Get()) < 0 || byte < 0 || byte > 255 ||
                    std::floor(byte) != byte)
                    InvalidValue("Bytes require a dense array of exact integers in [0,255].");
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned>(byte)));
            }
            return ScriptValue{std::move(bytes)};
        }

        ScriptValue Boolean(JSContext *context, JSValueConst value) {
            if (!JS_IsBool(value))
                InvalidValue("Boolean required.");
            return ScriptValue{JS_ToBool(context, value) != 0};
        }

        ScriptValue Number(JSContext *context, JSValueConst value) {
            double number{};
            if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) < 0 || !std::isfinite(number))
                InvalidValue("Finite Number required.");
            return ScriptValue{number};
        }

        ScriptValue StringValue(JSContext *context, JSValueConst value, Budget &budget, std::size_t depth) {
            if (!JS_IsString(value))
                InvalidValue("String required.");
            auto text = PrimitiveString(context, value, budget.limits.maximumStringBytes);
            budget.Charge(depth, text.size());
            return ScriptValue{std::move(text)};
        }

        ScriptValue ReadHandle(Values &values, JSValueConst value) {
            if (JS_IsProxy(value) || JS_GetClassID(value) != values.handleClass)
                InvalidValue("A host-issued opaque handle is required.");
            const auto *handle = static_cast<HandleBox *>(JS_GetOpaque(value, values.handleClass));
            if (!handle)
                InvalidValue("Opaque handle is no longer present.");
            if (auto valid = values.authority->ValidateHandle(handle->handle); valid.HasError())
                throw ConversionFailure{valid.ErrorValue()};
            return ScriptValue{handle->handle};
        }
    }  // namespace

    ScriptValue Values::ReadPrimitive(JSValueConst value, ScriptExportPrimitiveKind type, Budget &budget, std::size_t depth) {
        using P = ScriptExportPrimitiveKind;
        switch (type) {
            case P::Boolean:
                return Boolean(context, value);
            case P::SignedInteger:
                return ScriptValue{Signed(context, value)};
            case P::UnsignedInteger:
                return ScriptValue{Unsigned(context, value)};
            case P::Number:
                return Number(context, value);
            case P::String:
                return StringValue(context, value, budget, depth);
            case P::Bytes:
                return ReadBytes(*this, value, budget);
            case P::Handle:
                return ReadHandle(*this, value);
            default:
                InvalidValue("Unknown primitive type.");
        }
    }

    ScriptValue Values::Read(JSValueConst value, const ScriptExportTypeReference &type, const ScriptExportDescriptor &api, Budget &budget,
                             std::size_t depth) {
        budget.Charge(depth);
        if (JS_IsNull(value)) {
            if (type.nullability != ScriptExportNullability::Nullable)
                InvalidValue("Null is not allowed for this field.");
            return ScriptValue::Null();
        }
        if (type.primitive != ScriptExportPrimitiveKind::Count)
            return ReadPrimitive(value, type.primitive, budget, depth);
        const auto found = std::ranges::find(api.types, type.namedType, &ScriptExportTypeDescriptor::id);
        if (found == api.types.end())
            InvalidValue("Named type is absent from the exact admitted descriptor.");
        return ReadNamed(value, *found, api, budget, depth);
    }

    ScriptValue Values::ReadNamed(JSValueConst value, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api,
                                  Budget &budget, std::size_t depth) {
        switch (type.kind) {
            using enum Horo::Extensions::ScriptExportNamedTypeKind;
            case Enum:
                return ScriptValue{ScriptEnumValue{type.id, Signed(context, value)}};
            case Array:
                return ReadArray(value, type, api, budget, depth);
            case Map:
                return ReadMap(value, type, api, budget, depth);
            case Struct:
                return ReadStruct(value, type, api, budget, depth);
            default:
                InvalidValue("Unsupported named type.");
        }
    }

    ScriptValue Values::ReadArray(JSValueConst value, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api,
                                  Budget &budget, std::size_t depth) {
        const auto count = Length(value, budget);
        if (count > std::min(type.maximumElements, budget.limits.maximumArrayElements))
            InvalidValue("Array exceeds the declared element bound.");
        ScriptValue::ArrayElements result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto child = Index(*this, value, static_cast<std::uint32_t>(i));
            result.push_back(Read(child.Get(), type.elementType, api, budget, depth + 1));
        }
        return ScriptValue::Array(std::move(result));
    }

    ScriptValue Values::ReadMap(JSValueConst value, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api,
                                Budget &budget, std::size_t depth) {
        const auto count = Length(value, budget);
        if (count > std::min(type.maximumElements, budget.limits.maximumMapEntries))
            InvalidValue("Map exceeds the declared entry bound.");
        ScriptValue::MapEntries result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto pair = Index(*this, value, static_cast<std::uint32_t>(i));
            if (Length(pair.Get(), budget) != 2)
                InvalidValue("Map entries require exactly [key,value].");
            auto key = Index(*this, pair.Get(), 0);
            auto item = Index(*this, pair.Get(), 1);
            result.emplace_back(Read(key.Get(), type.keyType, api, budget, depth + 1),
                                Read(item.Get(), type.valueType, api, budget, depth + 1));
        }
        return ScriptValue::Map(std::move(result));
    }

    ScriptValue Values::ReadStruct(JSValueConst value, const ScriptExportTypeDescriptor &type, const ScriptExportDescriptor &api,
                                   Budget &budget, std::size_t depth) {
        Plain(value, false);
        auto keys = Keys(value, budget);
        if (keys.size() > budget.limits.maximumStructFields)
            InvalidValue("Struct field count exceeds the limit.");
        ScriptValue::StructFields fields;
        fields.reserve(keys.size());
        for (const auto &key : keys) {
            const auto field = std::ranges::find(type.fields, key, &ScriptExportFieldDescriptor::id);
            if (field == type.fields.end())
                InvalidValue("Unknown own struct field.");
            auto child = Field(*this, value, key);
            fields.emplace_back(key, Read(child.Get(), field->type, api, budget, depth + 1));
        }
        return ScriptValue::Struct(type.id, std::move(fields));
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
