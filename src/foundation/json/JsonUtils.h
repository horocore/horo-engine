#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Horo::Foundation {
    using Json = nlohmann::json;

    /** @brief Reports whether a JSON object contains only required and optional fields. */
    [[nodiscard]] inline bool HasAllowedFields(const Json &value, const std::initializer_list<std::string_view> required,
                                               const std::initializer_list<std::string_view> optional = {}) {
        if (!value.is_object() || value.size() < required.size() || value.size() > required.size() + optional.size())
            return false;
        for (const std::string_view field : required) {
            if (!value.contains(field))
                return false;
        }
        return std::ranges::all_of(value.items(), [&](const auto &entry) {
            const std::string_view name = entry.key();
            return std::ranges::find(required, name) != required.end() || std::ranges::find(optional, name) != optional.end();
        });
    }

    /** @brief Rejects duplicate object keys and excessive nesting during JSON parsing. */
    class JsonParseGuard final {
    public:
        explicit JsonParseGuard(const std::size_t maximumDepth) : maximumDepth_(maximumDepth) {}

        bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
            if (depth < 0 || static_cast<std::size_t>(depth) >= maximumDepth_) {
                tooDeep_ = true;
                return false;
            }
            const auto index =
                event == Json::parse_event_t::key && depth > 0 ? static_cast<std::size_t>(depth - 1) : static_cast<std::size_t>(depth);
            if (keys_.size() <= index)
                keys_.resize(index + 1);
            if (event == Json::parse_event_t::object_start)
                keys_[index].clear();
            if (event == Json::parse_event_t::key && !keys_[index].insert(value.get<std::string>()).second)
                duplicate_ = true;
            if (event == Json::parse_event_t::object_end)
                keys_[index].clear();
            return !duplicate_ && !tooDeep_;
        }

        [[nodiscard]] bool HasDuplicate() const noexcept {
            return duplicate_;
        }

        [[nodiscard]] bool IsTooDeep() const noexcept {
            return tooDeep_;
        }

    private:
        std::size_t maximumDepth_;
        std::vector<std::unordered_set<std::string>> keys_;
        bool duplicate_{};
        bool tooDeep_{};
    };
}  // namespace Horo::Foundation
