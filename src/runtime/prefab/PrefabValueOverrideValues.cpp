#include "Horo/Foundation/Utf8.h"
#include "PrefabValueOverridesInternal.h"

#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Prefab::ValueDetail {
    namespace {
        /** @brief Applies declared inclusive numeric constraints without losing integer precision. */
        bool NumberValid(const long double value, const PrefabValuePropertySchema *schema) {
            return std::isfinite(value) &&
                   (!schema || ((!schema->minimum || value >= *schema->minimum) && (!schema->maximum || value <= *schema->maximum)));
        }

        /** @brief Preserves exact finite components; only signed zero has one wire representation. */
        double Scalar(const double value) noexcept {
            return value == 0 ? 0 : value;
        }

        template <class Vector> bool VectorValid(const Vector &value, const PrefabValuePropertySchema *schema) {
            if (!NumberValid(value.x, schema) || !NumberValid(value.y, schema))
                return false;
            if constexpr (requires { value.z; })
                if (!NumberValid(value.z, schema))
                    return false;
            if constexpr (requires { value.w; })
                if (!NumberValid(value.w, schema))
                    return false;
            return true;
        }

        template <class Vector> Wire EncodeVector(const Vector &value) {
            auto result = Wire::array({Scalar(value.x), Scalar(value.y)});
            if constexpr (requires { value.z; })
                result.push_back(Scalar(value.z));
            if constexpr (requires { value.w; })
                result.push_back(Scalar(value.w));
            return result;
        }

        /** @brief Encodes local TRS using the same explicit component order as canonical prefab sources. */
        Wire EncodeTransform(const Math::Transform &value) {
            return Wire{{"translation", EncodeVector(value.translation)},
                        {"rotation", EncodeVector(value.rotation)},
                        {"scale", EncodeVector(value.scale)}};
        }

        template <class Vector, std::size_t Count> Result<PrefabOverrideValue> DecodeVector(const Json &value) {
            const auto numbers = Detail::ParseFloatArray<Count>(value);
            if (numbers.HasError())
                return Invalid<PrefabOverrideValue>("Invalid finite override vector.");
            Vector result{};
            result.x = numbers.Value()[0];
            result.y = numbers.Value()[1];
            if constexpr (Count > 2)
                result.z = numbers.Value()[2];
            if constexpr (Count > 3)
                result.w = numbers.Value()[3];
            return Result<PrefabOverrideValue>::Success(result);
        }
    }  // namespace

    /** @brief Validate complete value/type/range evidence before any derived component is written. */
    Result<void> ValidateValue(const PrefabOverrideValue &value, const PrefabValuePropertySchema *schema) {
        if (schema && (static_cast<std::size_t>(schema->kind) != value.index() || value.index() >= 7))
            return Invalid<void>("Override value does not match its registered property kind.");
        const bool valid = std::visit([schema]<class T>(const T &item) {
            if constexpr (std::is_same_v<T, bool>)
                return true;
            else if constexpr (std::is_arithmetic_v<T>)
                return NumberValid(item, schema);
            else if constexpr (std::is_same_v<T, std::string>)
                return item.size() <= (schema ? schema->maximumStringBytes : PrefabHardLimits::OverrideValueBytes) &&
                       IsValidUtf8ScalarSequence(item);
            else if constexpr (std::is_same_v<T, Math::Transform>)
                return item.TryToMatrix().HasValue();
            else if constexpr (std::is_same_v<T, Math::Quaternion>)
                return VectorValid(item, schema) && item.TryNormalized().HasValue();
            else
                return VectorValid(item, schema);
        }, value);
        return valid ? Result<void>::Success() : Invalid<void>("Override value is non-finite, out of range, malformed, or oversized.");
    }

    /** @brief Encode one closed typed value without ambiguous numeric or positional property identity. */
    Wire EncodeValue(const PrefabOverrideValue &value) {
        constexpr std::array names{"boolean", "integer", "number", "string", "vec2", "vec3", "quaternion", "transform"};
        auto payload = std::visit([]<class T>(const T &item) -> Wire {
            if constexpr (std::is_same_v<T, Math::Transform>)
                return EncodeTransform(item);
            else if constexpr (std::is_same_v<T, double>)
                return Scalar(item);
            else if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
                return item;
            else
                return EncodeVector(item);
        }, value);
        return Wire{{"kind", names[value.index()]}, {"value", std::move(payload)}};
    }

    /** @brief Decode exact numeric kinds without narrowing large integer payloads through floating point. */
    Result<PrefabOverrideValue> DecodeValue(const Json &value) {
        if (!Foundation::HasAllowedFields(value, {"kind", "value"}) || !value["kind"].is_string())
            return Invalid<PrefabOverrideValue>("Override value envelope is invalid.");
        const auto &kind = value["kind"].get_ref<const std::string &>();
        const auto &payload = value["value"];
        if (kind == "boolean" && payload.is_boolean())
            return Result<PrefabOverrideValue>::Success(payload.get<bool>());
        if (kind == "integer" && payload.is_number_integer() &&
            (!payload.is_number_unsigned() || payload.get<std::uint64_t>() <= std::numeric_limits<std::int64_t>::max()))
            return Result<PrefabOverrideValue>::Success(payload.get<std::int64_t>());
        if (kind == "number" && payload.is_number()) {
            const auto decoded = Detail::ReadDouble(payload);
            return decoded.HasValue() ? Result<PrefabOverrideValue>::Success(decoded.Value()) : Invalid<PrefabOverrideValue>();
        }
        if (kind == "string" && payload.is_string())
            return Result<PrefabOverrideValue>::Success(payload.get<std::string>());
        if (kind == "vec2")
            return DecodeVector<Math::Vec2, 2>(payload);
        if (kind == "vec3")
            return DecodeVector<Math::Vec3, 3>(payload);
        if (kind == "quaternion")
            return DecodeVector<Math::Quaternion, 4>(payload);
        if (kind == "transform") {
            const auto decoded = Detail::ParseTransform(payload);
            return decoded.HasValue() ? Result<PrefabOverrideValue>::Success(decoded.Value()) : Invalid<PrefabOverrideValue>();
        }
        return Invalid<PrefabOverrideValue>("Unknown or malformed typed override value.");
    }

    /** @brief Hash schema-canonical semantic value bytes, never JSON presentation text. */
    Sha256Digest ValueDigest(const PrefabOverrideValue &value) {
        const std::string bytes = EncodeValue(value).dump(0);
        return ComputeSha256(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    /** @brief Compare canonical values exactly; no Inspector display epsilon or current-default substitution. */
    bool SameValue(const PrefabOverrideValue &left, const PrefabOverrideValue &right) {
        return EncodeValue(left).dump(0) == EncodeValue(right).dump(0);
    }

    /** @brief Bridge actual canonical component payloads through the registered kind. */
    Result<PrefabOverrideValue> ReadPayloadValue(const Json &value, const Gameplay::ComponentPropertyKind kind) {
        constexpr std::array names{"boolean", "integer", "number", "string", "vec2", "vec3", "quaternion"};
        if (static_cast<std::size_t>(kind) >= names.size())
            return Invalid<PrefabOverrideValue>();
        return DecodeValue(Json{{"kind", names[static_cast<std::size_t>(kind)]}, {"value", value}});
    }

    /** @brief Write a validated value through its canonical component payload representation. */
    Json PayloadValue(const PrefabOverrideValue &value) {
        return EncodeValue(value).ToJson()["value"];
    }
}  // namespace Horo::Prefab::ValueDetail
