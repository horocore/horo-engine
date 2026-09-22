#include "Horo/Extensions/ScriptValue.h"

#include <algorithm>
#include <utility>

namespace Horo::Extensions {
    namespace {
        [[nodiscard]] bool EqualMaps(const ScriptValue &left, const ScriptValue &right) noexcept {
            if (left.AsMap().size() != right.AsMap().size())
                return false;
            for (const auto &[entryKey, entryValue] : left.AsMap()) {
                const auto found = std::ranges::find_if(right.AsMap(), [&entryKey](const auto &candidate) {
                    return entryKey == candidate.first;
                });
                if (found == right.AsMap().end() || entryValue != found->second)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool EqualStructs(const ScriptValue &left, const ScriptValue &right) noexcept {
            if (left.StructType() != right.StructType() || left.AsStruct().size() != right.AsStruct().size())
                return false;
            for (const auto &[fieldId, fieldValue] : left.AsStruct()) {
                const auto found = std::ranges::find_if(right.AsStruct(), [&fieldId](const auto &candidate) {
                    return fieldId == candidate.first;
                });
                if (found == right.AsStruct().end() || fieldValue != found->second)
                    return false;
            }
            return true;
        }
    }  // namespace

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
        return ScriptValue{std::make_shared<const ScriptArray>(std::move(values))};
    }

    /** @copydoc ScriptValue::Map */
    ScriptValue ScriptValue::Map(MapEntries values) {
        return ScriptValue{std::make_shared<const ScriptMap>(std::move(values))};
    }

    /** @copydoc ScriptValue::Struct */
    ScriptValue ScriptValue::Struct(std::string type, StructFields fields) {
        return ScriptValue{std::make_shared<const ScriptStruct>(std::move(type), std::move(fields))};
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
                return std::ranges::equal(AsBytes(), other.AsBytes());
            case Handle:
                return *AsHandle() == *other.AsHandle();
            case Enum:
                return *AsEnum() == *other.AsEnum();
            case Array:
                return std::ranges::equal(AsArray(), other.AsArray());
            case Map:
                return EqualMaps(*this, other);
            case Struct:
                return EqualStructs(*this, other);
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
