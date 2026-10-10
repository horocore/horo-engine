#pragma once

#include "Horo/Foundation/Utf8.h"
#include "Horo/UiTemplates/UiTemplateDocumentOwner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>
#include <type_traits>

namespace Horo::UiTemplates::Detail::Codec {
    using Json = nlohmann::json;

    /** @brief Private typed unwind across codecs; public operations return the preserved Horo error. */
    struct Failure final {
        Error error;
    };

    /** @brief Rejects malformed source without echoing authored values in diagnostics. */
    [[noreturn]] inline void Reject() {
        throw Failure{MakeError(UiErrors::TemplatePersistenceMalformed)};
    }

    /** @brief Transfers a validated factory result without weakening its ownership or error contract. */
    template <typename T> T Take(Result<T> result) {
        if (result.HasError())
            throw Failure{result.ErrorValue()};
        return std::move(result).Value();
    }

    /** @brief Requires the complete closed object shape, rejecting missing and unknown semantic fields. */
    inline void Fields(const Json &value, const std::initializer_list<std::string_view> keys) {
        if (!value.is_object() || value.size() != keys.size())
            Reject();
        for (const auto key : keys)
            if (!value.contains(std::string{key}))
                Reject();
    }

    /** @brief Bounds array cardinality before caller reserve/copy operations. */
    inline const Json &Array(const Json &value, const std::size_t maximum) {
        if (!value.is_array() || value.size() > maximum)
            Reject();
        return value;
    }

    /** @brief Admits exact integer representations and checked narrowing; floats never become integers. */
    template <typename T> T Integer(const Json &value) {
        if (!value.is_number_integer())
            Reject();
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
                Reject();
            return static_cast<T>(number);
        }
        const auto number = value.get<std::int64_t>();
        if constexpr (std::is_unsigned_v<T>) {
            if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<T>::max())
                Reject();
        } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max())
            Reject();
        return static_cast<T>(number);
    }

    /** @brief Requires a finite scalar representable in the destination width. */
    template <typename T> T Scalar(const Json &value) {
        if (!value.is_number())
            Reject();
        const double number = value.get<double>();
        if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<T>::max())
            Reject();
        return static_cast<T>(number);
    }

    /** @brief Admits UTF-8 source text without interpreting markup, paths or property expressions. */
    inline std::string Text(const Json &value, const std::size_t maximum = 4096) {
        if (!value.is_string())
            Reject();
        const auto &text = value.get_ref<const std::string &>();
        if (text.size() > maximum || !IsValidUtf8ScalarSequence(text))
            Reject();
        return text;
    }

    /** @brief Reads one closed enum without accepting unknown future semantic alternatives. */
    template <typename T> T Enum(const Json &value, const std::uint8_t count) {
        const auto ordinal = Integer<std::uint8_t>(value);
        if (ordinal >= count)
            Reject();
        return static_cast<T>(ordinal);
    }

    /** @brief Reads an exact Boolean without numeric/string coercion. */
    inline bool Boolean(const Json &value) {
        if (!value.is_boolean())
            Reject();
        return value.get<bool>();
    }

    /** @brief Emits a fixed-width canonical lowercase byte encoding. */
    inline std::string Hex(const std::span<const std::uint8_t> bytes) {
        constexpr std::string_view digits{"0123456789abcdef"};
        std::string text(bytes.size() * 2, '0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            text[i * 2] = digits[bytes[i] >> 4];
            text[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return text;
    }

    /** @brief Reads only canonical fixed-width hex; malformed identities are never truncated or padded. */
    template <std::size_t N> std::array<std::uint8_t, N> Bytes(const Json &value) {
        const auto text = Text(value, N * 2);
        if (text.size() != N * 2)
            Reject();
        constexpr std::string_view digits{"0123456789abcdef"};
        std::array<std::uint8_t, N> bytes{};
        for (std::size_t i = 0; i < N; ++i) {
            const auto high = digits.find(text[i * 2]);
            const auto low = digits.find(text[i * 2 + 1]);
            if (high == digits.npos || low == digits.npos)
                Reject();
            bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return bytes;
    }

    /** @brief Decodes a strong UI identity, allowing the reserved empty value only for explicitly optional fields. */
    template <typename T> T Identity(const Json &value, const bool optional = false) {
        const auto bytes = Bytes<16>(value);
        if (optional && std::ranges::all_of(bytes, [](const auto byte) {
            return byte == 0;
        }))
            return {};
        return Take(T::Create(bytes));
    }

    /** @brief Decodes stable Assets identity bytes; the owning typed validator determines requiredness. */
    inline Assets::AssetId Asset(const Json &value) {
        return Assets::AssetId::FromBytes(Bytes<16>(value));
    }

    /** @brief Reads a bounded nullable scalar field without changing absence to a default. */
    template <typename T> std::optional<T> OptionalInteger(const Json &value) {
        return value.is_null() ? std::nullopt : std::optional<T>{Integer<T>(value)};
    }

    /** @brief Reads nullable finite scalar bounds. */
    inline std::optional<double> OptionalScalar(const Json &value) {
        return value.is_null() ? std::nullopt : std::optional<double>{Scalar<double>(value)};
    }

    /** @brief Encodes exact absence separately from a present scalar. */
    template <typename T> Json Optional(const std::optional<T> &value) {
        return value ? Json(*value) : Json(nullptr);
    }

    /** @brief Requires an exact fixed-width tuple of typed semantic fields. */
    inline const Json &Tuple(const Json &value, const std::size_t count) {
        if (Array(value, count).size() != count)
            Reject();
        return value;
    }

    /** @brief Closed typed source codecs; all returned values own their payloads. */
    Json WriteReference(const Runtime::Ui::UiReference &value);
    Runtime::Ui::UiReference ReadReference(const Json &value);
    Json WriteStyle(const Runtime::Ui::UiStyleValue &value);
    Runtime::Ui::UiStyleValue ReadStyle(const Json &value);
    Json WriteLocalized(const Runtime::Ui::LocalizedMessageRef &value);
    Runtime::Ui::LocalizedMessageRef ReadLocalized(const Json &value);
    Json WriteBinding(const Runtime::Ui::UiBindingDescriptor &value);
    Runtime::Ui::UiBindingDescriptor ReadBinding(const Json &value);
    Json WriteValue(const UiTemplateValue &value);
    UiTemplateValue ReadValue(const Json &value);
    Json WriteAccepted(const UiTemplateReference &value);
    UiTemplateReference ReadAccepted(const Json &value);
    Json WriteRecords(std::span<const UiTemplateInstanceRecord> records);
    std::vector<UiTemplateInstanceRecord> ReadRecords(const Json &value, const UiTemplateLimits &limits);
    Json WriteRich(const UiTemplateAuthoredDocument &source);
    void ReadRich(const Json &value, UiTemplateAuthoredDocument &source, const UiTemplateLimits &limits);
    /** @brief Encodes exact registered schemas, including defaults/effects and binding signature/version evidence. */
    Json WriteSchemas(const UiTemplateSchemaSnapshot &schemas);
}  // namespace Horo::UiTemplates::Detail::Codec
