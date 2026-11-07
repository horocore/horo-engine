#include "Horo/Extensions/ExtensionErrors.h"
#include "JavaScriptModuleValues.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace Horo::Extensions::JavaScriptModuleDetail {
    [[noreturn]] void InvalidValue(std::string_view reason) {
        throw ConversionFailure{MakeError(ExtensionErrors::ScriptValueTypeMismatch, std::string(reason))};
    }

    JSValue Checked(JSValue value) {
        if (JS_IsException(value))
            InvalidValue("JavaScript allocation or value operation failed.");
        return value;
    }

    void Budget::Charge(std::size_t depth, std::size_t payload) {
        if (deadline && std::chrono::steady_clock::now() >= *deadline)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptInvocationTimeout)};
        if (depth > limits.maximumDepth || nodes >= std::min(limits.maximumWorkUnits, limits.maximumElements) ||
            payload > limits.maximumBytes - bytes)
            throw ConversionFailure{MakeError(ExtensionErrors::ScriptValueCapacityExceeded)};
        ++nodes;
        bytes += payload;
    }

    namespace {
        bool ScalarString(const std::uint16_t *text, std::size_t length) {
            for (std::size_t index = 0; index < length; ++index) {
                const auto unit = text[index];
                if (unit < 0xD800 || unit > 0xDFFF)
                    continue;
                if (unit >= 0xDC00 || index + 1 == length)
                    return false;
                const auto next = text[++index];
                if (next < 0xDC00 || next > 0xDFFF)
                    return false;
            }
            return true;
        }

        void Unicode(JSContext *context, JSValueConst value, std::size_t maximum) {
            std::size_t length{};
            const auto *text = JS_ToCStringLenUTF16(context, &length, value);
            if (!text)
                InvalidValue("Primitive string conversion failed.");

            struct Release {
                JSContext *context;
                const std::uint16_t *text;

                ~Release() {
                    JS_FreeCStringUTF16(context, text);
                }
            } release{context, text};

            if (length > maximum || !ScalarString(text, length))
                InvalidValue("String exceeds its bound or contains an unmatched surrogate.");
        }
    }  // namespace

    std::string PrimitiveString(JSContext *context, JSValueConst value, std::size_t maximum) {
        if (!JS_IsString(value))
            InvalidValue("Only a primitive string may be converted to text.");
        Unicode(context, value, maximum);
        std::size_t length{};
        const char *text = JS_ToCStringLen(context, &length, value);
        if (!text)
            InvalidValue("Primitive text conversion failed.");

        struct Release {
            JSContext *context;
            const char *text;

            ~Release() {
                JS_FreeCString(context, text);
            }
        } release{context, text};

        if (length > maximum)
            InvalidValue("Primitive text exceeds its bound.");
        return std::string(text, length);
    }

    std::int64_t Signed(JSContext *context, JSValueConst value) {
        if (!JS_IsBigInt(value))
            InvalidValue("An exact signed BigInt is required.");
        std::int64_t result{};
        if (JS_ToBigInt64(context, &result, value) < 0)
            InvalidValue("BigInt conversion failed.");
        Value roundtrip{context, Checked(JS_NewBigInt64(context, result))};
        if (!JS_IsStrictEqual(context, value, roundtrip.Get()))
            InvalidValue("BigInt is outside the signed 64-bit range.");
        return result;
    }

    std::uint64_t Unsigned(JSContext *context, JSValueConst value) {
        if (!JS_IsBigInt(value))
            InvalidValue("An exact unsigned BigInt is required.");
        std::uint64_t result{};
        if (JS_ToBigUint64(context, &result, value) < 0)
            InvalidValue("BigInt conversion failed.");
        Value roundtrip{context, Checked(JS_NewBigUint64(context, result))};
        if (!JS_IsStrictEqual(context, value, roundtrip.Get()))
            InvalidValue("BigInt is outside the unsigned 64-bit range.");
        return result;
    }

    void Values::Plain(JSValueConst value, bool array) const {
        if (!JS_IsObject(value) || JS_IsProxy(value) || JS_GetClassID(value) != (array ? arrayClass : objectClass))
            InvalidValue("Only an ordinary nonproxy plain object or dense array is admitted.");
    }

    Value Values::Own(JSValueConst object, JSAtom atom) const {
        JSPropertyDescriptor descriptor{};
        const int found = JS_GetOwnProperty(context, &descriptor, object, atom);
        if (found <= 0)
            InvalidValue("A required own data property is absent.");
        Value value{context, descriptor.value};
        Value getter{context, descriptor.getter};
        Value setter{context, descriptor.setter};
        if ((descriptor.flags & JS_PROP_TMASK) != JS_PROP_NORMAL)
            InvalidValue("Accessors and lazy properties are not data.");
        return value;
    }

    std::vector<std::string> Values::Keys(JSValueConst object, Budget &budget) const {
        JSPropertyEnum *table{};
        std::uint32_t length{};
        if (JS_GetOwnPropertyNames(context, &table, &length, object, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
            InvalidValue("Own property enumeration failed.");

        struct Release {
            JSContext *context;
            JSPropertyEnum *table;
            std::uint32_t length;

            ~Release() {
                JS_FreePropertyEnum(context, table, length);
            }
        } release{context, table, length};

        if (length > budget.limits.maximumElements)
            InvalidValue("Object property count exceeds the finite limit.");
        std::vector<std::string> result;
        result.reserve(length);
        for (std::uint32_t i = 0; i < length; ++i) {
            Value key{context, Checked(JS_AtomToValue(context, table[i].atom))};
            if (!JS_IsString(key.Get()))
                InvalidValue("Symbol properties cannot cross a script boundary.");
            auto name = PrimitiveString(context, key.Get(), budget.limits.maximumStringBytes);
            budget.Charge(0, name.size());
            auto data = Own(object, table[i].atom);
            result.push_back(std::move(name));
        }
        return result;
    }

    std::size_t Values::Length(JSValueConst object, Budget &budget) const {
        Plain(object, true);
        auto keys = Keys(object, budget);
        JSAtom atom = JS_NewAtom(context, "length");
        auto length = Own(object, atom);
        JS_FreeAtom(context, atom);
        double count{};
        if (!JS_IsNumber(length.Get()) || JS_ToFloat64(context, &count, length.Get()) < 0)
            InvalidValue("Invalid array length.");
        if (count < 0 || count > static_cast<double>(budget.limits.maximumElements) || std::floor(count) != count)
            InvalidValue("Array exceeds the finite bound.");
        const auto size = static_cast<std::size_t>(count);
        if (keys.size() != size + 1)
            InvalidValue("Arrays must be dense and have no additional properties.");
        return size;
    }
}  // namespace Horo::Extensions::JavaScriptModuleDetail
