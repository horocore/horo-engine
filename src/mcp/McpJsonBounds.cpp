#include "McpJsonBounds.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace Horo::Mcp {
    namespace {
        /** @brief Saturates untrusted size accounting at the declared budget. */
        [[nodiscard]] bool AddWithin(std::size_t &total, const std::size_t amount, const std::size_t maximum) noexcept {
            if (amount > maximum - total)
                return false;
            total += amount;
            return true;
        }

        /** @brief Counts JSON string bytes exactly for the default UTF-8, non-ASCII-preserving serializer. */
        [[nodiscard]] std::size_t EscapedSize(const std::string_view text) noexcept {
            std::size_t bytes = 2;
            for (const unsigned char value : text) {
                if (value == '\b' || value == '\f' || value == '\n' || value == '\r' || value == '\t' || value == '"' || value == '\\')
                    bytes += 2;
                else if (value < 0x20U)
                    bytes += 6;
                else
                    ++bytes;
            }
            return bytes;
        }

        struct Budget {
            std::size_t nodes{};
            std::size_t strings{};
            std::size_t encoded{};
        };

        /** @brief Traverses a bounded JSON tree; no recursion occurs beyond maximumDepth. */
        [[nodiscard]] bool Visit(const nlohmann::json &value, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes,
                                 const std::size_t depth, Budget &budget) {
            if (depth > limits.maximumDepth || !AddWithin(budget.nodes, 1, limits.maximumNodes))
                return false;
            if (value.is_string()) {
                const auto &text = value.get_ref<const std::string &>();
                return IsValidUtf8ScalarSequence(text) && AddWithin(budget.strings, text.size(), limits.maximumStringBytes) &&
                       AddWithin(budget.encoded, EscapedSize(text), maximumEncodedBytes);
            }
            if (value.is_array()) {
                if (!AddWithin(budget.encoded, 2 + (value.empty() ? 0 : value.size() - 1), maximumEncodedBytes))
                    return false;
                for (const auto &item : value)
                    if (!Visit(item, limits, maximumEncodedBytes, depth + 1, budget))
                        return false;
                return true;
            }
            if (value.is_object()) {
                if (!AddWithin(budget.encoded, 2 + (value.empty() ? 0 : value.size() - 1), maximumEncodedBytes))
                    return false;
                for (auto item = value.begin(); item != value.end(); ++item) {
                    if (!IsValidUtf8ScalarSequence(item.key()) ||
                        !AddWithin(budget.strings, item.key().size(), limits.maximumStringBytes) ||
                        !AddWithin(budget.encoded, EscapedSize(item.key()) + 1, maximumEncodedBytes) ||
                        !Visit(item.value(), limits, maximumEncodedBytes, depth + 1, budget))
                        return false;
                }
                return true;
            }
            if (value.is_number_float() && !std::isfinite(value.get<nlohmann::json::number_float_t>()))
                return false;
            if (value.is_null() || value.is_boolean() || value.is_number())
                return AddWithin(budget.encoded, value.is_number() ? 32 : (value.is_boolean() ? 5 : 4), maximumEncodedBytes);
            return false;
        }
    }  // namespace

    /** @copydoc FrameWithinBounds */
    bool FrameWithinBounds(const std::string_view frame, const McpSessionLimits &limits) noexcept {
        if (frame.size() > limits.maximumFrameBytes)
            return false;
        std::size_t depth = 0;
        std::size_t nodes = 0;
        std::size_t stringBytes = 0;
        bool quoted = false;
        bool escaped = false;
        bool primitive = false;
        for (const unsigned char value : frame) {
            if (quoted) {
                if (escaped) {
                    escaped = false;
                    ++stringBytes;
                } else if (value == '\\') {
                    escaped = true;
                    ++stringBytes;
                } else if (value == '"') {
                    quoted = false;
                } else {
                    ++stringBytes;
                }
                if (stringBytes > limits.maximumStringBytes)
                    return false;
                continue;
            }
            if (value == '"') {
                quoted = true;
                primitive = false;
                if (++nodes > limits.maximumNodes)
                    return false;
            } else if (value == '{' || value == '[') {
                primitive = false;
                if (++depth > limits.maximumDepth || ++nodes > limits.maximumNodes)
                    return false;
            } else if (value == '}' || value == ']') {
                primitive = false;
                if (depth != 0)
                    --depth;
            } else if (value == ',' || value == ':' || std::isspace(value) != 0) {
                primitive = false;
            } else if (!primitive) {
                primitive = true;
                if (++nodes > limits.maximumNodes)
                    return false;
            }
        }
        return true;
    }

    /** @copydoc JsonWithinBounds */
    bool JsonWithinBounds(const nlohmann::json &value, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes) {
        Budget budget;
        return Visit(value, limits, maximumEncodedBytes, 1, budget);
    }

    /** @copydoc ValidRequestId */
    bool ValidRequestId(const nlohmann::json &id, const McpSessionLimits &limits) noexcept {
        return (id.is_string() && !id.get_ref<const std::string &>().empty() &&
                id.get_ref<const std::string &>().size() <= limits.maximumIdentityBytes &&
                IsValidUtf8ScalarSequence(id.get_ref<const std::string &>())) ||
               id.is_number_integer();
    }
}  // namespace Horo::Mcp
