#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
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
            return Observe(depth, event, event == Json::parse_event_t::key ? value.get_ref<const Json::string_t &>() : std::string_view{});
        }

        /** @brief Validates keys and depth with SAX before constructing an ordinary JSON DOM.
         * @param first Start of stable, multipass JSON text bytes.
         * @param last End of the same immutable byte range.
         * @return Parsed DOM, or a discarded value when HasDuplicate/IsTooDeep reports rejection.
         * @throws Json::exception for malformed JSON, preserving the ordinary parser's diagnostics.
         * @details Avoids callback DOM construction, whose discarded-value cleanup scans growing arrays.
         * Validation stops before DOM allocation on duplicate/depth rejection; syntax validation and DOM
         * construction use the same pinned JSON parser. Input must remain unchanged through both passes.
         */
        template <std::forward_iterator Iterator> [[nodiscard]] Json Parse(Iterator first, Iterator last);

        [[nodiscard]] bool HasDuplicate() const noexcept {
            return duplicate_;
        }

        [[nodiscard]] bool IsTooDeep() const noexcept {
            return tooDeep_;
        }

    private:
        /** @brief Shares exactly the callback guard's event depth and per-object key lifetime. */
        bool Observe(const int depth, const Json::parse_event_t event, const std::string_view key = {}) {
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
            if (event == Json::parse_event_t::key && !keys_[index].emplace(key).second)
                duplicate_ = true;
            if (event == Json::parse_event_t::object_end)
                keys_[index].clear();
            return !duplicate_ && !tooDeep_;
        }

        /** @brief Public SAX protocol without constructing or discarding partial DOM nodes. */
        class SaxValidation final {
        public:
            explicit SaxValidation(JsonParseGuard &guard) : guard_(guard) {}

            bool null() {
                return Value();
            }

            bool boolean(bool) {
                return Value();
            }

            bool number_integer(Json::number_integer_t) {
                return Value();
            }

            bool number_unsigned(Json::number_unsigned_t) {
                return Value();
            }

            bool number_float(Json::number_float_t, const Json::string_t &) {
                return Value();
            }

            bool string(Json::string_t &) {
                return Value();
            }

            bool binary(Json::binary_t &) {
                return Value();
            }

            bool key(const Json::string_t &value) {
                return guard_.Observe(depth_, Json::parse_event_t::key, value);
            }

            bool start_object(std::size_t) {
                return Start(Json::parse_event_t::object_start);
            }

            bool end_object() {
                return End(Json::parse_event_t::object_end);
            }

            bool start_array(std::size_t) {
                return Start(Json::parse_event_t::array_start);
            }

            bool end_array() {
                return End(Json::parse_event_t::array_end);
            }

            template <typename Exception> bool parse_error(std::size_t, const Json::string_t &, const Exception &) const {
                return false;
            }

        private:
            bool Value() {
                return guard_.Observe(depth_, Json::parse_event_t::value);
            }

            bool Start(const Json::parse_event_t event) {
                if (!guard_.Observe(depth_, event))
                    return false;
                ++depth_;
                return true;
            }

            bool End(const Json::parse_event_t event) {
                --depth_;
                return guard_.Observe(depth_, event);
            }

            JsonParseGuard &guard_;
            int depth_{};
        };

        std::size_t maximumDepth_;
        std::vector<std::unordered_set<std::string>> keys_;
        bool duplicate_{};
        bool tooDeep_{};
    };

    /** @copydoc JsonParseGuard::Parse */
    template <std::forward_iterator Iterator> Json JsonParseGuard::Parse(const Iterator first, const Iterator last) {
        SaxValidation validation{*this};
        // A syntax failure is replayed by ordinary parsing below to preserve its exception type and position.
        static_cast<void>(Json::sax_parse(first, last, &validation));
        if (HasDuplicate() || IsTooDeep())
            return Json(Json::value_t::discarded);
        return Json::parse(first, last);
    }
}  // namespace Horo::Foundation
