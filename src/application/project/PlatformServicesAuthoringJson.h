#pragma once

/** @file PlatformServicesAuthoringJson.h
 * @brief Private bounded JSON primitives for project-owned Platform Services documents.
 */

#include "Horo/Foundation/Result.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Application::PlatformServicesAuthoringJson {
    using Json = nlohmann::ordered_json;

    /** @brief Adds safe source/field context without echoing private or malformed authoring values. */
    inline Error AuthoringError(const ErrorCodeDescriptor &descriptor, const char *source, const char *path, const char *reason) {
        auto error = MakeError(descriptor);
        error.diagnostics.push_back({.code = DiagnosticCode{descriptor.code.Value()},
                                     .message = std::string{reason} + " " + std::string{descriptor.remediationHint},
                                     .location = {.source = source},
                                     .path = path});
        return error;
    }

    /** @brief Requires every member of a closed schema before accessing any field. */
    inline bool Fields(const Json &value, const std::initializer_list<std::string_view> fields) {
        return value.is_object() && value.size() == fields.size() && std::ranges::all_of(fields, [&](const auto field) {
            return value.contains(field);
        });
    }

    /** @brief Reads only bounded string values; numbers never stand in for stable IDs. */
    inline std::string Text(const Json &value, const std::size_t maximum) {
        if (!value.is_string() || value.get_ref<const std::string &>().size() > maximum)
            throw std::invalid_argument("bounded text required");
        return value.get<std::string>();
    }

    /** @brief Rejects negative, fractional and unknown integer kind/version representations. */
    inline std::uint32_t Number(const Json &value, const std::uint32_t maximum) {
        if (!value.is_number_unsigned())
            throw std::invalid_argument("unsigned integer required");
        const auto number = value.get<std::uint64_t>();
        if (number > maximum)
            throw std::invalid_argument("integer capacity exceeded");
        return static_cast<std::uint32_t>(number);
    }

    /** @brief Parses bounded JSON while rejecting duplicate members and deep nesting before projection. */
    inline Json Document(const std::string_view source, const std::size_t maximumMembers = 32768) {
        std::vector<std::set<std::string>> keys;
        std::size_t keyCount{};
        const auto callback = [&](const int depth, const Json::parse_event_t event, const Json &value) {
            if (depth > 8)
                throw std::invalid_argument("depth exceeded");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            if (event == Json::parse_event_t::key) {
                const auto &key = value.get_ref<const std::string &>();
                if (key.size() > 64 || ++keyCount > maximumMembers)
                    throw std::invalid_argument("member capacity exceeded");
                if (!keys.back().insert(key).second)
                    throw std::invalid_argument("duplicate member");
            }
            if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        };
        return Json::parse(source, callback);
    }

}  // namespace Horo::Application::PlatformServicesAuthoringJson
