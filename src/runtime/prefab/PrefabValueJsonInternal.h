#pragma once
/** @file PrefabValueJsonInternal.h
 * @brief One runtime-safe canonical scalar/transform/revision codec shared by
 * source and cooked delta adapters.
 */
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabResolvedObject.h"
#include "Horo/Prefab/PrefabSourceRevision.h"
#include "JsonUtils.h"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>

namespace Horo::Prefab::Detail {
    using Json = nlohmann::json;

    /** @brief Creates one bounded prefab source error without exposing parser
     * implementation details. */
    [[nodiscard]] inline Error Failure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return MakeError(descriptor, std::move(message));
    }

    /** @brief Reports whether a public project version is in canonical
     * parser-produced form. */
    [[nodiscard]] inline bool IsCanonicalProjectVersion(const Application::HoroVersion &version) {
        const auto parsed = Application::ParseHoroVersion(Application::FormatHoroVersion(version));
        return parsed.HasValue() && parsed.Value() == version;
    }

    template <typename T> [[nodiscard]] Result<T> Failed(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
        return Result<T>::Failure(Failure(descriptor, std::move(message)));
    }

    using Horo::Foundation::HasAllowedFields;
    using Horo::Foundation::JsonParseGuard;
    [[nodiscard]] Result<std::string> ReadString(const Json &value);

    template <typename Integer> [[nodiscard]] Result<Integer> ReadUnsigned(const Json &value) {
        if (!value.is_number_unsigned())
            return Failed<Integer>(PrefabErrors::DocumentInvalid, "Prefab source integer is not unsigned.");
        const auto decoded = value.get<std::uint64_t>();
        if (decoded > std::numeric_limits<Integer>::max())
            return Failed<Integer>(PrefabErrors::DocumentInvalid, "Prefab source integer is outside its encoded range.");
        return Result<Integer>::Success(static_cast<Integer>(decoded));
    }

    [[nodiscard]] Result<float> ReadFloat(const Json &value);
    [[nodiscard]] Result<double> ReadDouble(const Json &value);
    [[nodiscard]] Result<Assets::AssetId> ParseAssetId(const Json &value);
    [[nodiscard]] Result<Application::HoroVersion> ParseProjectVersion(const Json &value);
    [[nodiscard]] Result<std::optional<LocalObjectId>> ParseOptionalLocalObjectId(const Json &value);

    template <std::size_t Count> [[nodiscard]] Result<std::array<float, Count>> ParseFloatArray(const Json &value) {
        if (!value.is_array() || value.size() != Count)
            return Failed<std::array<float, Count>>(PrefabErrors::DocumentInvalid, "Prefab transform array has the wrong length.");
        std::array<float, Count> result{};
        for (std::size_t index = 0; index < Count; ++index) {
            auto parsed = ReadFloat(value.at(index));
            if (parsed.HasError())
                return Result<std::array<float, Count>>::Failure(parsed.ErrorValue());
            result[index] = parsed.Value();
        }
        return Result<std::array<float, Count>>::Success(result);
    }

    [[nodiscard]] Result<Math::Transform> ParseTransform(const Json &value);
    [[nodiscard]] Result<PrefabSourceRevision> ParseRevision(const Json &value);
    [[nodiscard]] Result<std::vector<std::byte>> ParseComponentBytes(const Json &value);
}  // namespace Horo::Prefab::Detail
