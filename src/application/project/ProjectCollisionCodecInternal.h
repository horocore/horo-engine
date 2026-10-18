#pragma once

#include "Horo/Application/ProjectCollisionSettings.h"
#include "JsonUtils.h"

#include <limits>
#include <stdexcept>

namespace Horo::Application::CollisionCodecInternal {
    using Foundation::Json;

    /** @brief Requires a closed semantic object shape before any field access. */
    inline void Fields(const Json &value, std::initializer_list<std::string_view> fields) {
        if (!Foundation::HasAllowedFields(value, fields))
            throw std::invalid_argument("Incomplete or unknown collision document fields.");
    }

    /** @brief Reads an exact bounded unsigned value without narrowing/coercion. */
    inline std::uint64_t Unsigned(const Json &value, std::uint64_t maximum = UINT64_MAX) {
        if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
            throw std::invalid_argument("Unsigned collision integer required.");
        const auto result = value.get<std::uint64_t>();
        if (result > maximum)
            throw std::invalid_argument("Collision integer capacity exceeded.");
        return result;
    }

    /** @brief Requires a real boolean; numeric and string replacements are invalid. */
    inline bool Boolean(const Json &value) {
        if (!value.is_boolean())
            throw std::invalid_argument("Collision boolean required.");
        return value.get<bool>();
    }

    /** @brief Parses one exact tagged canonical UUID without cross-type conversion. */
    template <typename Id> Id Identity(const Json &value) {
        if (!value.is_string())
            throw std::invalid_argument("Collision UUID required.");
        auto parsed = Id::Parse(value.get<std::string>());
        if (parsed.HasError())
            throw std::invalid_argument("Invalid collision UUID.");
        return parsed.Value();
    }

    /** @brief Requires a bounded array before reserving or copying decoded definitions. */
    inline void Array(const Json &value, std::size_t maximum) {
        if (!value.is_array() || value.size() > maximum)
            throw std::invalid_argument("Collision array capacity exceeded.");
    }

    /** @brief Reads complete owned semantic rows. @param root Closed admitted document. @return Complete authored schema. */
    Physics::ProjectCollisionSchema ReadSchema(const Json &root);
    /** @brief Writes canonical normalized rows. @param settings Validated document. @return Complete JSON schema object. */
    Json WriteSchema(const ProjectCollisionSettings &settings);
}  // namespace Horo::Application::CollisionCodecInternal
