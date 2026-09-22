#pragma once

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptValue.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Detail {
    inline constexpr std::uint8_t ValueMagic0 = 0x53;
    inline constexpr std::uint8_t ValueMagic1 = 0x56;
    inline constexpr std::uint8_t ValueCodecVersion = 1;
    inline constexpr std::uint8_t ResultMagic0 = 0x53;
    inline constexpr std::uint8_t ResultMagic1 = 0x52;
    inline constexpr std::uint8_t ResultCodecVersion = 1;

    [[nodiscard]] inline Error MakeBoundaryError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return MakeError(descriptor, std::move(message));
    }

    template <typename T> [[nodiscard]] inline Result<T> CapacityExceeded(std::string message) {
        return Result<T>::Failure(MakeBoundaryError(ExtensionErrors::ScriptValueCapacityExceeded, std::move(message)));
    }

    template <typename T> [[nodiscard]] inline Result<T> InvalidEncoding(std::string message) {
        return Result<T>::Failure(MakeBoundaryError(ExtensionErrors::ScriptValueEncodingInvalid, std::move(message)));
    }

    [[nodiscard]] inline bool IsScalarMapKey(ScriptValue::Kind kind) noexcept {
        using enum ScriptValue::Kind;
        switch (kind) {
            case Boolean:
            case SignedInteger:
            case UnsignedInteger:
            case Number:
            case String:
            case Bytes:
            case Enum:
                return true;
            case Null:
            case Handle:
            case Array:
            case Map:
            case Struct:
            case Count:
                return false;
        }
        return false;
    }

    struct Encoder final {
        std::vector<std::byte> &output;
        const ScriptValueLimits &limits;
        std::size_t work{};

        [[nodiscard]] bool CanAppend(std::size_t count) const noexcept;
        [[nodiscard]] bool Consume(std::size_t count = 1) noexcept;
        [[nodiscard]] bool AppendByte(std::uint8_t value);
        [[nodiscard]] bool AppendBytes(std::span<const std::byte> bytes);
        [[nodiscard]] bool AppendU64(std::uint64_t value);
        [[nodiscard]] bool AppendString(std::string_view value);
        [[nodiscard]] bool EncodeValue(const ScriptValue &value);
        [[nodiscard]] bool EncodeArrayValue(const ScriptValue &value);
        [[nodiscard]] bool EncodeMapValue(const ScriptValue &value);
        [[nodiscard]] bool EncodeStructValue(const ScriptValue &value);
    };

    struct Decoder final {
        std::span<const std::byte> input;
        const ScriptValueLimits &limits;
        std::size_t position{};
        std::size_t work{};
        std::size_t elements{};
        std::size_t rawBytes{};

        [[nodiscard]] bool Consume(std::size_t count = 1) noexcept;
        [[nodiscard]] bool CanRead(std::size_t count) const noexcept;
        [[nodiscard]] bool ReadByte(std::uint8_t &value) noexcept;
        [[nodiscard]] bool ReadU64(std::uint64_t &value) noexcept;
        [[nodiscard]] bool ReadLength(std::uint64_t encoded, std::size_t maximum, std::size_t &length) const noexcept;
        [[nodiscard]] bool ReadString(std::string &value);
        [[nodiscard]] bool ReadBytes(ScriptValue::Bytes &value);
        [[nodiscard]] bool AddElements(std::size_t count) noexcept;
        [[nodiscard]] Result<ScriptValue> DecodeValue(std::size_t depth);
        [[nodiscard]] Result<ScriptValue> DecodeNullOrBoolean(std::uint8_t tag) const;
        [[nodiscard]] Result<ScriptValue> DecodeInteger(bool signedInteger);
        [[nodiscard]] Result<ScriptValue> DecodeNumber();
        [[nodiscard]] Result<ScriptValue> DecodeStringValue();
        [[nodiscard]] Result<ScriptValue> DecodeBytesValue();
        [[nodiscard]] Result<ScriptValue> DecodeHandleValue();
        [[nodiscard]] Result<ScriptValue> DecodeEnumValue();
        [[nodiscard]] Result<ScriptValue> DecodeArrayValue(std::size_t depth);
        [[nodiscard]] Result<ScriptValue> DecodeMapValue(std::size_t depth);
        [[nodiscard]] Result<ScriptValue> DecodeStructValue(std::size_t depth);
    };

    [[nodiscard]] bool EncodeError(Encoder &encoder, const ScriptError &error);
    [[nodiscard]] Result<ScriptError> DecodeError(Decoder &decoder);
}  // namespace Horo::Extensions::Detail
