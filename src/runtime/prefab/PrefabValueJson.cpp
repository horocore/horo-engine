#include "Horo/Foundation/Utf8.h"
#include "PrefabValueJsonInternal.h"

#include <cmath>

namespace Horo::Prefab::Detail {
    /** @brief Parses one UTF-8 JSON string while retaining only scalar-valid text.
     */
    [[nodiscard]] Result<std::string> ReadString(const Json &value) {
        if (!value.is_string())
            return Failed<std::string>(PrefabErrors::DocumentInvalid, "Prefab source field must be a string.");
        std::string decoded = value.get<std::string>();
        if (!IsValidUtf8ScalarSequence(decoded))
            return Failed<std::string>(PrefabErrors::DocumentInvalid, "Prefab source contains invalid UTF-8 text.");
        return Result<std::string>::Success(std::move(decoded));
    }

    /** @brief Parses one finite single-precision JSON number without accepting
     * overflow. */
    [[nodiscard]] Result<float> ReadFloat(const Json &value) {
        if (!value.is_number())
            return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is not numeric.");
        const double decoded = value.get<double>();
        if (!std::isfinite(decoded) || std::abs(decoded) > std::numeric_limits<float>::max())
            return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is non-finite or out of range.");
        const auto converted = static_cast<float>(decoded);
        if (!std::isfinite(converted))
            return Failed<float>(PrefabErrors::DocumentInvalid, "Prefab transform value is non-finite.");
        return Result<float>::Success(converted);
    }

    /** @brief Parses one finite double JSON number. */
    [[nodiscard]] Result<double> ReadDouble(const Json &value) {
        if (!value.is_number())
            return Failed<double>(PrefabErrors::DocumentInvalid, "Prefab behavior value is not numeric.");
        const double decoded = value.get<double>();
        if (!std::isfinite(decoded))
            return Failed<double>(PrefabErrors::DocumentInvalid, "Prefab behavior value is non-finite.");
        return Result<double>::Success(decoded);
    }

    /** @brief Parses a canonical Asset Registry identity and rejects path-shaped
     * references. */
    [[nodiscard]] Result<Assets::AssetId> ParseAssetId(const Json &value) {
        auto text = ReadString(value);
        if (text.HasError())
            return Result<Assets::AssetId>::Failure(text.ErrorValue());
        auto parsed = Assets::AssetId::Parse(text.Value());
        if (parsed.HasError() || parsed.Value().ToString() != text.Value())
            return Failed<Assets::AssetId>(PrefabErrors::ReferenceInvalid, "Prefab references must use canonical AssetId text.");
        return parsed;
    }

    /** @brief Parses the one unified project version used by all durable project
     * assets. */
    [[nodiscard]] Result<Application::HoroVersion> ParseProjectVersion(const Json &value) {
        auto text = ReadString(value);
        if (text.HasError())
            return Result<Application::HoroVersion>::Failure(text.ErrorValue());
        auto parsed = Application::ParseHoroVersion(text.Value());
        if (parsed.HasError() || Application::FormatHoroVersion(parsed.Value()) != text.Value())
            return Failed<Application::HoroVersion>(PrefabErrors::UnsupportedPrefabSchema,
                                                    "Prefab projectVersion is not canonical or supported.");
        return parsed;
    }

    /** @brief Parses one optional root-inclusive local object identity. */
    [[nodiscard]] Result<std::optional<LocalObjectId>> ParseOptionalLocalObjectId(const Json &value) {
        if (value.is_null())
            return Result<std::optional<LocalObjectId>>::Success(std::nullopt);
        auto parsed = ReadUnsigned<std::uint32_t>(value);
        if (parsed.HasError())
            return Result<std::optional<LocalObjectId>>::Failure(parsed.ErrorValue());
        return Result<std::optional<LocalObjectId>>::Success(LocalObjectId{parsed.Value()});
    }

    /** @brief Parses the backend-neutral transform object used by every hierarchy
     * record. */
    [[nodiscard]] Result<Math::Transform> ParseTransform(const Json &value) {
        if (!HasAllowedFields(value, {"translation", "rotation", "scale"}))
            return Failed<Math::Transform>(PrefabErrors::DocumentInvalid, "Prefab transform fields are not canonical.");
        auto translation = ParseFloatArray<3>(value.at("translation"));
        auto rotation = ParseFloatArray<4>(value.at("rotation"));
        auto scale = ParseFloatArray<3>(value.at("scale"));
        if (translation.HasError())
            return Result<Math::Transform>::Failure(translation.ErrorValue());
        if (rotation.HasError())
            return Result<Math::Transform>::Failure(rotation.ErrorValue());
        if (scale.HasError())
            return Result<Math::Transform>::Failure(scale.ErrorValue());
        return Result<Math::Transform>::Success(
            {.translation = {translation.Value()[0], translation.Value()[1], translation.Value()[2]},
             .rotation = {rotation.Value()[0], rotation.Value()[1], rotation.Value()[2], rotation.Value()[3]},
             .scale = {scale.Value()[0], scale.Value()[1], scale.Value()[2]}});
    }

    /** @brief Parses an exact source revision without introducing a prefab-specific
     * version counter. */
    [[nodiscard]] Result<PrefabSourceRevision> ParseRevision(const Json &value) {
        if (!HasAllowedFields(value, {"projectVersion", "contentDigest"}))
            return Failed<PrefabSourceRevision>(PrefabErrors::DocumentInvalid, "Prefab source revision fields are not canonical.");
        auto version = ParseProjectVersion(value.at("projectVersion"));
        if (version.HasError())
            return Result<PrefabSourceRevision>::Failure(version.ErrorValue());
        auto digestText = ReadString(value.at("contentDigest"));
        if (digestText.HasError())
            return Result<PrefabSourceRevision>::Failure(digestText.ErrorValue());
        auto digest = ParseSha256(digestText.Value());
        if (digest.HasError())
            return Failed<PrefabSourceRevision>(PrefabErrors::DocumentInvalid, "Prefab source revision digest is not canonical.");
        return Result<PrefabSourceRevision>::Success(
            {.projectVersion = std::move(version).Value(), .contentDigest = std::move(digest).Value()});
    }

    /** @brief Parses bounded opaque component bytes from the canonical byte-array
     * envelope. */
    [[nodiscard]] Result<std::vector<std::byte>> ParseComponentBytes(const Json &value) {
        if (!HasAllowedFields(value, {"bytes"}) || !value.at("bytes").is_array() ||
            value.at("bytes").size() > Gameplay::MaximumSerializedComponentBytes)
            return Failed<std::vector<std::byte>>(PrefabErrors::PayloadTooLarge, "Prefab component payload exceeds its byte bound.");
        const Json &encoded = value.at("bytes");
        std::vector<std::byte> bytes(encoded.size());
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            const Json &entry = encoded.at(index);
            auto byte = ReadUnsigned<std::uint16_t>(entry);
            if (byte.HasError())
                return Failed<std::vector<std::byte>>(PrefabErrors::DocumentInvalid, "Prefab component payload contains a non-byte value.");
            if (byte.Value() > std::numeric_limits<std::uint8_t>::max())
                return Failed<std::vector<std::byte>>(PrefabErrors::DocumentInvalid,
                                                      "Prefab component payload contains an oversized byte.");
            bytes[index] = static_cast<std::byte>(byte.Value());
        }
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    }

}  // namespace Horo::Prefab::Detail
