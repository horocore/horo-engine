#include "Horo/Extensions/ScriptValue.h"

#include <algorithm>
#include <utility>

namespace Horo::Extensions {
    /** @copydoc ScriptHandle::IsValid */
    bool ScriptHandle::IsValid() const noexcept {
        return context.IsValid() && providerGeneration != 0 && value != 0 && generation != 0 && !type.empty();
    }

    ScriptValue::ScriptValue() noexcept : value_(std::monostate{}) {}

    ScriptValue::ScriptValue(bool value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::int64_t value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::uint64_t value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(double value) noexcept : value_(value) {}

    ScriptValue::ScriptValue(std::string value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(Bytes value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(ScriptHandle value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(ScriptEnumValue value) : value_(std::move(value)) {}

    ScriptValue::ScriptValue(Storage value) noexcept : value_(std::move(value)) {}

    /** @copydoc ScriptValue::Null */
    ScriptValue ScriptValue::Null() noexcept {
        return ScriptValue{};
    }

    /** @copydoc ScriptValue::String */
    ScriptValue ScriptValue::String(std::string_view value) {
        return ScriptValue{std::string{value}};
    }

    /** @copydoc ScriptValue::BytesValue */
    ScriptValue ScriptValue::BytesValue(std::span<const std::byte> value) {
        return ScriptValue{Bytes{value.begin(), value.end()}};
    }

    /** @copydoc ScriptValue::Array */
    ScriptValue ScriptValue::Array(ArrayElements values) {
        return ScriptValue{std::make_shared<const ScriptArray>(ScriptArray{std::move(values)})};
    }

    /** @copydoc ScriptValue::Map */
    ScriptValue ScriptValue::Map(MapEntries values) {
        return ScriptValue{std::make_shared<const ScriptMap>(ScriptMap{std::move(values)})};
    }

    /** @copydoc ScriptValue::Struct */
    ScriptValue ScriptValue::Struct(std::string type, StructFields fields) {
        return ScriptValue{std::make_shared<const ScriptStruct>(ScriptStruct{std::move(type), std::move(fields)})};
    }

    /** @copydoc ScriptValue::GetKind */
    ScriptValue::Kind ScriptValue::GetKind() const noexcept {
        switch (value_.index()) {
            case 0:
                return Kind::Null;
            case 1:
                return Kind::Boolean;
            case 2:
                return Kind::SignedInteger;
            case 3:
                return Kind::UnsignedInteger;
            case 4:
                return Kind::Number;
            case 5:
                return Kind::String;
            case 6:
                return Kind::Bytes;
            case 7:
                return Kind::Handle;
            case 8:
                return Kind::Enum;
            case 9:
                return Kind::Array;
            case 10:
                return Kind::Map;
            case 11:
                return Kind::Struct;
            default:
                return Kind::Count;
        }
    }

    /** @copydoc ScriptValue::AsBoolean */
    const bool *ScriptValue::AsBoolean() const noexcept {
        return std::get_if<bool>(&value_);
    }

    /** @copydoc ScriptValue::AsSignedInteger */
    const std::int64_t *ScriptValue::AsSignedInteger() const noexcept {
        return std::get_if<std::int64_t>(&value_);
    }

    /** @copydoc ScriptValue::AsUnsignedInteger */
    const std::uint64_t *ScriptValue::AsUnsignedInteger() const noexcept {
        return std::get_if<std::uint64_t>(&value_);
    }

    /** @copydoc ScriptValue::AsNumber */
    const double *ScriptValue::AsNumber() const noexcept {
        return std::get_if<double>(&value_);
    }

    /** @copydoc ScriptValue::AsString */
    std::string_view ScriptValue::AsString() const noexcept {
        if (const auto *value = std::get_if<std::string>(&value_))
            return *value;
        return {};
    }

    /** @copydoc ScriptValue::AsBytes */
    std::span<const std::byte> ScriptValue::AsBytes() const noexcept {
        if (const auto *value = std::get_if<Bytes>(&value_))
            return *value;
        return {};
    }

    /** @copydoc ScriptValue::AsHandle */
    const ScriptHandle *ScriptValue::AsHandle() const noexcept {
        return std::get_if<ScriptHandle>(&value_);
    }

    /** @copydoc ScriptValue::AsEnum */
    const ScriptEnumValue *ScriptValue::AsEnum() const noexcept {
        return std::get_if<ScriptEnumValue>(&value_);
    }

    /** @copydoc ScriptValue::AsArray */
    std::span<const ScriptValue> ScriptValue::AsArray() const noexcept {
        if (const auto *value = std::get_if<ArrayStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->values;
        return {};
    }

    /** @copydoc ScriptValue::AsMap */
    std::span<const ScriptValue::MapEntry> ScriptValue::AsMap() const noexcept {
        if (const auto *value = std::get_if<MapStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->values;
        return {};
    }

    /** @copydoc ScriptValue::StructType */
    std::string_view ScriptValue::StructType() const noexcept {
        if (const auto *value = std::get_if<StructStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->type;
        return {};
    }

    /** @copydoc ScriptValue::AsStruct */
    std::span<const ScriptValue::StructField> ScriptValue::AsStruct() const noexcept {
        if (const auto *value = std::get_if<StructStorage>(&value_); value != nullptr && *value != nullptr)
            return (*value)->fields;
        return {};
    }

    /** @copydoc ScriptValue::IsNull */
    bool ScriptValue::IsNull() const noexcept {
        return GetKind() == Kind::Null;
    }

    /** @copydoc ScriptValue::operator== */
    bool ScriptValue::operator==(const ScriptValue &other) const noexcept {
        if (GetKind() != other.GetKind())
            return false;
        using enum Kind;
        switch (GetKind()) {
            case Null:
                return true;
            case Boolean:
                return *AsBoolean() == *other.AsBoolean();
            case SignedInteger:
                return *AsSignedInteger() == *other.AsSignedInteger();
            case UnsignedInteger:
                return *AsUnsignedInteger() == *other.AsUnsignedInteger();
            case Number:
                return *AsNumber() == *other.AsNumber();
            case String:
                return AsString() == other.AsString();
            case Bytes:
                return std::equal(AsBytes().begin(), AsBytes().end(), other.AsBytes().begin(), other.AsBytes().end());
            case Handle:
                return *AsHandle() == *other.AsHandle();
            case Enum:
                return *AsEnum() == *other.AsEnum();
            case Array:
                return std::equal(AsArray().begin(), AsArray().end(), other.AsArray().begin(), other.AsArray().end());
            case Map:
                if (AsMap().size() != other.AsMap().size())
                    return false;
                for (const auto &left : AsMap()) {
                    const auto found = std::find_if(other.AsMap().begin(), other.AsMap().end(), [&left](const MapEntry &right) {
                        return left.first == right.first;
                    });
                    if (found == other.AsMap().end() || left.second != found->second)
                        return false;
                }
                return true;
            case Struct:
                if (StructType() != other.StructType() || AsStruct().size() != other.AsStruct().size())
                    return false;
                for (const auto &left : AsStruct()) {
                    const auto found = std::find_if(other.AsStruct().begin(), other.AsStruct().end(), [&left](const StructField &right) {
                        return left.first == right.first;
                    });
                    if (found == other.AsStruct().end() || left.second != found->second)
                        return false;
                }
                return true;
            case Count:
                return false;
        }
        return false;
    }

    /** @copydoc ScriptCallResult::Success */
    ScriptCallResult ScriptCallResult::Success(std::vector<ScriptValue> values) {
        return ScriptCallResult{.values = std::move(values), .error = std::nullopt};
    }

    /** @copydoc ScriptCallResult::Failure */
    ScriptCallResult ScriptCallResult::Failure(ScriptError error) {
        return ScriptCallResult{.values = {}, .error = std::move(error)};
    }

    /** @copydoc ScriptCallResult::IsSuccess */
    bool ScriptCallResult::IsSuccess() const noexcept {
        return !error.has_value();
    }

    /** @copydoc ScriptCallResult::IsFailure */
    bool ScriptCallResult::IsFailure() const noexcept {
        return error.has_value();
    }
}  // namespace Horo::Extensions
