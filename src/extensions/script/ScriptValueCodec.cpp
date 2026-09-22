#include "ScriptValueCodecInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace Horo::Extensions::Detail {
    using namespace ExtensionErrors;

    bool Encoder::CanAppend(std::size_t count) const noexcept {
        return count <= limits.maximumEncodedBytes - std::min(output.size(), limits.maximumEncodedBytes);
    }

    bool Encoder::Consume(std::size_t count) noexcept {
        if (count > limits.maximumWorkUnits - std::min(work, limits.maximumWorkUnits))
            return false;
        work += count;
        return true;
    }

    bool Encoder::AppendByte(std::uint8_t value) {
        if (!CanAppend(1))
            return false;
        output.push_back(static_cast<std::byte>(value));
        return true;
    }

    bool Encoder::AppendBytes(std::span<const std::byte> bytes) {
        if (!CanAppend(bytes.size()))
            return false;
        output.insert(output.end(), bytes.begin(), bytes.end());
        return true;
    }

    bool Encoder::AppendU64(std::uint64_t value) {
        if (!CanAppend(sizeof(value)))
            return false;
        for (std::size_t index = 0; index < sizeof(value); ++index)
            output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
        return true;
    }

    bool Encoder::AppendString(std::string_view value) {
        return value.size() <= limits.maximumStringBytes && AppendU64(value.size()) &&
               AppendBytes(std::as_bytes(std::span{value.data(), value.size()}));
    }

    bool Decoder::Consume(std::size_t count) noexcept {
        if (count > limits.maximumWorkUnits - std::min(work, limits.maximumWorkUnits))
            return false;
        work += count;
        return true;
    }

    bool Decoder::CanRead(std::size_t count) const noexcept {
        return count <= input.size() - std::min(position, input.size());
    }

    bool Decoder::ReadByte(std::uint8_t &value) noexcept {
        if (!CanRead(1))
            return false;
        value = std::to_integer<std::uint8_t>(input[position++]);
        return true;
    }

    bool Decoder::ReadU64(std::uint64_t &value) noexcept {
        if (!CanRead(sizeof(value)))
            return false;
        value = 0;
        for (std::size_t index = 0; index < sizeof(value); ++index)
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[position++])) << (index * 8U);
        return true;
    }

    bool Decoder::ReadLength(std::uint64_t encoded, std::size_t maximum, std::size_t &length) const noexcept {
        if (encoded > maximum || encoded > std::numeric_limits<std::size_t>::max())
            return false;
        length = static_cast<std::size_t>(encoded);
        return true;
    }

    bool Decoder::ReadString(std::string &value) {
        std::uint64_t encodedLength{};
        if (!ReadU64(encodedLength))
            return false;
        std::size_t length{};
        if (!ReadLength(encodedLength, limits.maximumStringBytes, length) || !CanRead(length))
            return false;
        value.clear();
        value.reserve(length);
        for (const auto byte : input.subspan(position, length))
            value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        position += length;
        if (length > limits.maximumBytes - std::min(rawBytes, limits.maximumBytes))
            return false;
        rawBytes += length;
        return true;
    }

    bool Decoder::ReadBytes(ScriptValue::Bytes &value) {
        std::uint64_t encodedLength{};
        if (!ReadU64(encodedLength))
            return false;
        std::size_t length{};
        if (!ReadLength(encodedLength, limits.maximumBytes, length) || !CanRead(length))
            return false;
        value.assign(input.begin() + static_cast<std::ptrdiff_t>(position), input.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
        if (length > limits.maximumBytes - std::min(rawBytes, limits.maximumBytes))
            return false;
        rawBytes += length;
        return true;
    }

    bool Decoder::AddElements(std::size_t count) noexcept {
        if (count > limits.maximumElements - std::min(elements, limits.maximumElements))
            return false;
        elements += count;
        return true;
    }

    [[nodiscard]] bool Encoder::EncodeArrayValue(const ScriptValue &value) {
        const auto elements = value.AsArray();
        if (!AppendByte(10) || !AppendU64(elements.size()))
            return false;
        return std::ranges::all_of(elements, [this](const ScriptValue &element) {
            return EncodeValue(element);
        });
    }

    [[nodiscard]] bool Encoder::EncodeMapValue(const ScriptValue &value) {
        struct EncodedEntry final {
            std::vector<std::byte> key;
            std::vector<std::byte> value;
        };

        std::vector<EncodedEntry> encoded;
        encoded.reserve(value.AsMap().size());
        for (const auto &[key, mappedValue] : value.AsMap()) {
            encoded.emplace_back();
            Encoder keyEncoder{encoded.back().key, limits, work};
            if (!keyEncoder.EncodeValue(key))
                return false;
            work = keyEncoder.work;
            Encoder valueEncoder{encoded.back().value, limits, work};
            if (!valueEncoder.EncodeValue(mappedValue))
                return false;
            work = valueEncoder.work;
        }
        std::ranges::sort(encoded, [](const EncodedEntry &left, const EncodedEntry &right) {
            return std::ranges::lexicographical_compare(left.key, right.key);
        });
        for (std::size_t index = 1; index < encoded.size(); ++index) {
            if (encoded[index - 1].key == encoded[index].key)
                return false;
        }
        if (!AppendByte(11) || !AppendU64(encoded.size()))
            return false;
        return std::ranges::all_of(encoded, [this](const EncodedEntry &entry) {
            return AppendBytes(entry.key) && AppendBytes(entry.value);
        });
    }

    [[nodiscard]] bool Encoder::EncodeStructValue(const ScriptValue &value) {
        std::vector<std::size_t> order(value.AsStruct().size());
        for (std::size_t index = 0; index < order.size(); ++index)
            order[index] = index;
        std::ranges::sort(order, [&value](std::size_t left, std::size_t right) {
            return value.AsStruct()[left].first < value.AsStruct()[right].first;
        });
        for (std::size_t index = 1; index < order.size(); ++index) {
            if (value.AsStruct()[order[index - 1]].first == value.AsStruct()[order[index]].first)
                return false;
        }
        if (!AppendByte(12) || !AppendString(value.StructType()) || !AppendU64(order.size()))
            return false;
        return std::ranges::all_of(order, [this, &value](const auto index) {
            return AppendString(value.AsStruct()[index].first) && EncodeValue(value.AsStruct()[index].second);
        });
    }

    [[nodiscard]] bool Encoder::EncodeValue(const ScriptValue &value) {
        if (!Consume())
            return false;
        using enum ScriptValue::Kind;
        switch (value.GetKind()) {
            case Null:
                return AppendByte(0);
            case Boolean:
                return AppendByte(*value.AsBoolean() ? 2 : 1);
            case SignedInteger:
                return AppendByte(3) && AppendU64(static_cast<std::uint64_t>(*value.AsSignedInteger()));
            case UnsignedInteger:
                return AppendByte(4) && AppendU64(*value.AsUnsignedInteger());
            case Number:
                return AppendByte(5) && AppendU64(std::bit_cast<std::uint64_t>(*value.AsNumber()));
            case String:
                return AppendByte(6) && AppendString(value.AsString());
            case Bytes:
                return AppendByte(7) && value.AsBytes().size() <= limits.maximumBytes && AppendU64(value.AsBytes().size()) &&
                       AppendBytes(value.AsBytes());
            case Handle: {
                const auto &handle = *value.AsHandle();
                return AppendByte(8) && AppendU64(handle.context.value) && AppendU64(handle.providerGeneration) &&
                       AppendU64(handle.value) && AppendU64(handle.generation) && AppendString(handle.type);
            }
            case Enum: {
                const auto &enumerator = *value.AsEnum();
                return AppendByte(9) && AppendString(enumerator.type) && AppendU64(static_cast<std::uint64_t>(enumerator.value));
            }
            case Array:
                return EncodeArrayValue(value);
            case Map:
                return EncodeMapValue(value);
            case Struct:
                return EncodeStructValue(value);
            case Count:
                return false;
        }
        return false;
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeNullOrBoolean(std::uint8_t tag) const {
        if (tag == 0)
            return Result<ScriptValue>::Success(ScriptValue::Null());
        return Result<ScriptValue>::Success(ScriptValue{tag == 2});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeInteger(bool signedInteger) {
        std::uint64_t encoded{};
        if (!ReadU64(encoded))
            return InvalidEncoding<ScriptValue>(signedInteger ? "Script signed integer payload is truncated."
                                                              : "Script unsigned integer payload is truncated.");
        return signedInteger ? Result<ScriptValue>::Success(ScriptValue{static_cast<std::int64_t>(encoded)})
                             : Result<ScriptValue>::Success(ScriptValue{encoded});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeNumber() {
        std::uint64_t encoded{};
        if (!ReadU64(encoded))
            return InvalidEncoding<ScriptValue>("Script number payload is truncated.");
        const auto number = std::bit_cast<double>(encoded);
        if (!std::isfinite(number))
            return InvalidEncoding<ScriptValue>("Script number payload is not finite.");
        return Result<ScriptValue>::Success(ScriptValue{number});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeStringValue() {
        std::string value;
        if (!ReadString(value))
            return InvalidEncoding<ScriptValue>("Script string payload is truncated or oversized.");
        return Result<ScriptValue>::Success(ScriptValue::String(value));
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeBytesValue() {
        ScriptValue::Bytes value;
        if (!ReadBytes(value))
            return InvalidEncoding<ScriptValue>("Script bytes payload is truncated or oversized.");
        return Result<ScriptValue>::Success(ScriptValue{std::move(value)});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeHandleValue() {
        ScriptHandle handle;
        if (!ReadU64(handle.context.value) || !ReadU64(handle.providerGeneration) || !ReadU64(handle.value) ||
            !ReadU64(handle.generation) || !ReadString(handle.type))
            return InvalidEncoding<ScriptValue>("Script handle payload is truncated or malformed.");
        return Result<ScriptValue>::Success(ScriptValue{std::move(handle)});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeEnumValue() {
        ScriptEnumValue enumerator;
        std::uint64_t value{};
        if (!ReadString(enumerator.type) || !ReadU64(value))
            return InvalidEncoding<ScriptValue>("Script enum payload is truncated or malformed.");
        enumerator.value = static_cast<std::int64_t>(value);
        return Result<ScriptValue>::Success(ScriptValue{std::move(enumerator)});
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeArrayValue(std::size_t depth) {
        std::uint64_t encodedCount{};
        std::size_t count{};
        if (!ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumArrayElements, count) || !AddElements(count))
            return CapacityExceeded<ScriptValue>("Script array element bound was exceeded.");
        ScriptValue::ArrayElements values;
        values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto element = DecodeValue(depth + 1);
            if (element.HasError())
                return element;
            values.emplace_back(std::move(element).Value());
        }
        return Result<ScriptValue>::Success(ScriptValue::Array(std::move(values)));
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeMapValue(std::size_t depth) {
        std::uint64_t encodedCount{};
        std::size_t count{};
        if (!ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumMapEntries, count) || !AddElements(count))
            return CapacityExceeded<ScriptValue>("Script map entry bound was exceeded.");
        ScriptValue::MapEntries values;
        values.reserve(count);
        std::size_t previousKeyStart = 0;
        std::size_t previousKeyEnd = 0;
        for (std::size_t index = 0; index < count; ++index) {
            const std::size_t keyStart = position;
            auto key = DecodeValue(depth + 1);
            const std::size_t keyEnd = position;
            if (key.HasError())
                return key;
            if (!IsScalarMapKey(key.Value().GetKind()))
                return InvalidEncoding<ScriptValue>("Script map key is not a bounded scalar.");
            if (index > 0) {
                const auto previous = input.subspan(previousKeyStart, previousKeyEnd - previousKeyStart);
                const auto current = input.subspan(keyStart, keyEnd - keyStart);
                if (!std::ranges::lexicographical_compare(previous, current))
                    return InvalidEncoding<ScriptValue>("Script map keys are not in canonical order.");
            }
            auto mapped = DecodeValue(depth + 1);
            if (mapped.HasError())
                return mapped;
            values.emplace_back(std::move(key).Value(), std::move(mapped).Value());
            previousKeyStart = keyStart;
            previousKeyEnd = keyEnd;
        }
        return Result<ScriptValue>::Success(ScriptValue::Map(std::move(values)));
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeStructValue(std::size_t depth) {
        std::string type;
        std::uint64_t encodedCount{};
        std::size_t count{};
        if (!ReadString(type) || !ReadU64(encodedCount) || !ReadLength(encodedCount, limits.maximumStructFields, count) ||
            !AddElements(count))
            return CapacityExceeded<ScriptValue>("Script struct field bound was exceeded.");
        ScriptValue::StructFields fields;
        fields.reserve(count);
        std::string previousId;
        for (std::size_t index = 0; index < count; ++index) {
            std::string id;
            if (!ReadString(id) || (index > 0 && !(previousId < id)))
                return InvalidEncoding<ScriptValue>("Script struct fields are not in canonical order.");
            auto field = DecodeValue(depth + 1);
            if (field.HasError())
                return field;
            previousId = id;
            fields.emplace_back(std::move(id), std::move(field).Value());
        }
        return Result<ScriptValue>::Success(ScriptValue::Struct(std::move(type), std::move(fields)));
    }

    [[nodiscard]] Result<ScriptValue> Decoder::DecodeValue(std::size_t depth) {
        if (depth > limits.maximumDepth || !Consume())
            return CapacityExceeded<ScriptValue>("Script value decode depth or work bound was exceeded.");
        std::uint8_t tag{};
        if (!ReadByte(tag))
            return InvalidEncoding<ScriptValue>("Script value payload ended before its tag.");
        switch (tag) {
            case 0:
            case 1:
            case 2:
                return DecodeNullOrBoolean(tag);
            case 3:
                return DecodeInteger(true);
            case 4:
                return DecodeInteger(false);
            case 5:
                return DecodeNumber();
            case 6:
                return DecodeStringValue();
            case 7:
                return DecodeBytesValue();
            case 8:
                return DecodeHandleValue();
            case 9:
                return DecodeEnumValue();
            case 10:
                return DecodeArrayValue(depth);
            case 11:
                return DecodeMapValue(depth);
            case 12:
                return DecodeStructValue(depth);
            default:
                return InvalidEncoding<ScriptValue>("Script value tag is unknown.");
        }
    }

    [[nodiscard]] bool AppendResultString(Encoder &encoder, std::string_view value) {
        return encoder.AppendString(value);
    }

    [[nodiscard]] bool EncodeError(Encoder &encoder, const ScriptError &error) {
        if (!AppendResultString(encoder, error.domain) || !AppendResultString(encoder, error.code) ||
            !AppendResultString(encoder, error.message) || !encoder.AppendByte(error.retryable ? 1 : 0) ||
            !encoder.AppendByte(error.cancelled ? 1 : 0) || !encoder.AppendU64(error.details.size()))
            return false;
        for (const auto &detail : error.details) {
            if (!AppendResultString(encoder, detail.id) || !encoder.EncodeValue(detail.value))
                return false;
        }
        return true;
    }

    [[nodiscard]] Result<ScriptError> DecodeError(Decoder &decoder) {
        ScriptError error;
        std::uint8_t retryable{};
        std::uint8_t cancelled{};
        std::uint64_t encodedCount{};
        std::size_t count{};
        if (!decoder.ReadString(error.domain) || !decoder.ReadString(error.code) || !decoder.ReadString(error.message) ||
            !decoder.ReadByte(retryable) || !decoder.ReadByte(cancelled) || !decoder.ReadU64(encodedCount) ||
            !decoder.ReadLength(encodedCount, decoder.limits.maximumErrorDetails, count) || !decoder.AddElements(count) || retryable > 1 ||
            cancelled > 1)
            return InvalidEncoding<ScriptError>("Script error payload is truncated or malformed.");
        error.retryable = retryable != 0;
        error.cancelled = cancelled != 0;
        error.details.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            ScriptErrorDetail detail;
            if (!decoder.ReadString(detail.id))
                return InvalidEncoding<ScriptError>("Script error detail identity is truncated or malformed.");
            auto value = decoder.DecodeValue(0);
            if (value.HasError())
                return Result<ScriptError>::Failure(value.ErrorValue());
            detail.value = std::move(value).Value();
            error.details.emplace_back(std::move(detail));
        }
        return Result<ScriptError>::Success(std::move(error));
    }

}  // namespace Horo::Extensions::Detail
