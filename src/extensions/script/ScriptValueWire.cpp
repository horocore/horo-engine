#include "ScriptValueCodecInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Extensions {
    using namespace Detail;
    using namespace ExtensionErrors;

    /** @copydoc EncodeScriptValue */
    Result<std::vector<std::byte>> EncodeScriptValue(const ScriptValue &value, const ScriptValueLimits &limits) {
        auto valid = ValidateScriptValue(value, limits);
        if (valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        std::vector<std::byte> output;
        output.reserve(std::min<std::size_t>(limits.maximumEncodedBytes, 256));
        Encoder encoder{output, limits};
        if (!encoder.AppendByte(ValueMagic0) || !encoder.AppendByte(ValueMagic1) || !encoder.AppendByte(ValueCodecVersion) ||
            !encoder.EncodeValue(value))
            return CapacityExceeded<std::vector<std::byte>>("Script value encoded payload bound was exceeded.");
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }

    /** @copydoc DecodeScriptValue */
    Result<ScriptValue> DecodeScriptValue(std::span<const std::byte> bytes, const ScriptValueLimits &limits) {
        if (bytes.size() > limits.maximumEncodedBytes)
            return CapacityExceeded<ScriptValue>("Script value encoded payload bound was exceeded.");
        Decoder decoder{bytes, limits};
        std::uint8_t magic0{};
        std::uint8_t magic1{};
        std::uint8_t version{};
        if (!decoder.ReadByte(magic0) || !decoder.ReadByte(magic1) || !decoder.ReadByte(version) || magic0 != ValueMagic0 ||
            magic1 != ValueMagic1 || version != ValueCodecVersion)
            return InvalidEncoding<ScriptValue>("Script value codec header is invalid.");
        auto value = decoder.DecodeValue(0);
        if (value.HasError())
            return value;
        if (decoder.position != bytes.size())
            return InvalidEncoding<ScriptValue>("Script value payload has trailing bytes.");
        auto valid = ValidateScriptValue(value.Value(), limits);
        if (valid.HasError())
            return Result<ScriptValue>::Failure(valid.ErrorValue());
        return value;
    }

    /** @copydoc EncodeScriptCallResult */
    Result<std::vector<std::byte>> EncodeScriptCallResult(const ScriptCallResult &result, const ScriptValueLimits &limits) {
        auto valid = ValidateScriptCallResult(result, limits);
        if (valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
        std::vector<std::byte> output;
        output.reserve(std::min<std::size_t>(limits.maximumEncodedBytes, 256));
        Encoder encoder{output, limits};
        if (!encoder.AppendByte(ResultMagic0) || !encoder.AppendByte(ResultMagic1) || !encoder.AppendByte(ResultCodecVersion) ||
            !encoder.AppendByte(result.IsSuccess() ? 0 : 1))
            return CapacityExceeded<std::vector<std::byte>>("Script result encoded payload bound was exceeded.");
        if (result.IsSuccess()) {
            if (!encoder.AppendU64(result.values.size()))
                return CapacityExceeded<std::vector<std::byte>>("Script result value count exceeded its encoded bound.");
            for (const auto &value : result.values) {
                if (!encoder.EncodeValue(value))
                    return CapacityExceeded<std::vector<std::byte>>("Script result encoded payload bound was exceeded.");
            }
        } else if (!EncodeError(encoder, *result.error)) {
            return CapacityExceeded<std::vector<std::byte>>("Script result error payload exceeded its encoded bound.");
        }
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }

    /** @copydoc DecodeScriptCallResult */
    Result<ScriptCallResult> DecodeScriptCallResult(std::span<const std::byte> bytes, const ScriptValueLimits &limits) {
        if (bytes.size() > limits.maximumEncodedBytes)
            return CapacityExceeded<ScriptCallResult>("Script result encoded payload bound was exceeded.");
        Decoder decoder{bytes, limits};
        std::uint8_t magic0{};
        std::uint8_t magic1{};
        std::uint8_t version{};
        std::uint8_t kind{};
        if (!decoder.ReadByte(magic0) || !decoder.ReadByte(magic1) || !decoder.ReadByte(version) || !decoder.ReadByte(kind) ||
            magic0 != ResultMagic0 || magic1 != ResultMagic1 || version != ResultCodecVersion || kind > 1)
            return InvalidEncoding<ScriptCallResult>("Script result codec header is invalid.");
        ScriptCallResult result;
        if (kind == 0) {
            std::uint64_t encodedCount{};
            std::size_t count{};
            if (!decoder.ReadU64(encodedCount) || !decoder.ReadLength(encodedCount, limits.maximumElements, count) ||
                !decoder.AddElements(count))
                return CapacityExceeded<ScriptCallResult>("Script result value count exceeded its bound.");
            result.values.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                auto value = decoder.DecodeValue(0);
                if (value.HasError())
                    return Result<ScriptCallResult>::Failure(value.ErrorValue());
                result.values.emplace_back(std::move(value).Value());
            }
        } else {
            auto error = DecodeError(decoder);
            if (error.HasError())
                return Result<ScriptCallResult>::Failure(error.ErrorValue());
            result.error = std::move(error).Value();
        }
        if (decoder.position != bytes.size())
            return InvalidEncoding<ScriptCallResult>("Script result payload has trailing bytes.");
        auto valid = ValidateScriptCallResult(result, limits);
        if (valid.HasError())
            return Result<ScriptCallResult>::Failure(valid.ErrorValue());
        return Result<ScriptCallResult>::Success(std::move(result));
    }

}  // namespace Horo::Extensions
