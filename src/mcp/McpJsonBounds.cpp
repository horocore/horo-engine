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

        [[nodiscard]] bool Visit(const nlohmann::json &value, const McpSessionLimits &limits, std::size_t maximumEncodedBytes,
                                 std::size_t depth, Budget &budget);

        /** @brief Accounts for a string without materializing its encoded representation. */
        [[nodiscard]] bool VisitString(const std::string_view text, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes,
                                       Budget &budget) noexcept {
            return IsValidUtf8ScalarSequence(text) && AddWithin(budget.strings, text.size(), limits.maximumStringBytes) &&
                   AddWithin(budget.encoded, EscapedSize(text), maximumEncodedBytes);
        }

        /** @brief Visits array children within the same cumulative budget. */
        [[nodiscard]] bool VisitArray(const nlohmann::json &value, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes,
                                      const std::size_t depth, Budget &budget) {
            if (!AddWithin(budget.encoded, 2 + (value.empty() ? 0 : value.size() - 1), maximumEncodedBytes))
                return false;
            for (const auto &item : value) {
                if (!Visit(item, limits, maximumEncodedBytes, depth + 1, budget))
                    return false;
            }
            return true;
        }

        /** @brief Visits object keys and values within the same cumulative budget. */
        [[nodiscard]] bool VisitObject(const nlohmann::json &value, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes,
                                       const std::size_t depth, Budget &budget) {
            if (!AddWithin(budget.encoded, 2 + (value.empty() ? 0 : value.size() - 1), maximumEncodedBytes))
                return false;
            for (auto item = value.begin(); item != value.end(); ++item) {
                if (!IsValidUtf8ScalarSequence(item.key()))
                    return false;
                if (!AddWithin(budget.strings, item.key().size(), limits.maximumStringBytes))
                    return false;
                if (!AddWithin(budget.encoded, EscapedSize(item.key()) + 1, maximumEncodedBytes))
                    return false;
                if (!Visit(item.value(), limits, maximumEncodedBytes, depth + 1, budget))
                    return false;
            }
            return true;
        }

        /** @brief Traverses a bounded JSON tree; no recursion occurs beyond maximumDepth. */
        [[nodiscard]] bool Visit(const nlohmann::json &value, const McpSessionLimits &limits, const std::size_t maximumEncodedBytes,
                                 const std::size_t depth, Budget &budget) {
            if (depth > limits.maximumDepth || !AddWithin(budget.nodes, 1, limits.maximumNodes))
                return false;
            if (value.is_string())
                return VisitString(value.get_ref<const std::string &>(), limits, maximumEncodedBytes, budget);
            if (value.is_array())
                return VisitArray(value, limits, maximumEncodedBytes, depth, budget);
            if (value.is_object())
                return VisitObject(value, limits, maximumEncodedBytes, depth, budget);
            if (value.is_number_float() && !std::isfinite(value.get<nlohmann::json::number_float_t>()))
                return false;
            if (value.is_null() || value.is_boolean() || value.is_number()) {
                std::size_t encodedBytes = 4;
                if (value.is_boolean())
                    encodedBytes = 5;
                if (value.is_number())
                    encodedBytes = 32;
                return AddWithin(budget.encoded, encodedBytes, maximumEncodedBytes);
            }
            return false;
        }

        struct FrameScan {
            std::size_t depth{};
            std::size_t nodes{};
            std::size_t stringBytes{};
            bool quoted{};
            bool escaped{};
            bool primitive{};
        };

        /** @brief Counts bytes while inside a quoted token. */
        [[nodiscard]] bool VisitQuotedByte(const unsigned char value, const McpSessionLimits &limits, FrameScan &scan) noexcept {
            if (scan.escaped) {
                scan.escaped = false;
                ++scan.stringBytes;
            } else if (value == '\\') {
                scan.escaped = true;
                ++scan.stringBytes;
            } else if (value == '"') {
                scan.quoted = false;
            } else {
                ++scan.stringBytes;
            }
            return scan.stringBytes <= limits.maximumStringBytes;
        }

        /** @brief Counts structural and primitive tokens outside quoted text. */
        [[nodiscard]] bool VisitUnquotedByte(const unsigned char value, const McpSessionLimits &limits, FrameScan &scan) noexcept {
            if (value == '"') {
                scan.quoted = true;
                scan.primitive = false;
                return ++scan.nodes <= limits.maximumNodes;
            }
            if (value == '{' || value == '[') {
                scan.primitive = false;
                ++scan.depth;
                ++scan.nodes;
                return scan.depth <= limits.maximumDepth && scan.nodes <= limits.maximumNodes;
            }
            if (value == '}' || value == ']') {
                scan.primitive = false;
                if (scan.depth != 0)
                    --scan.depth;
                return true;
            }
            if (value == ',' || value == ':' || std::isspace(value) != 0) {
                scan.primitive = false;
                return true;
            }
            if (!scan.primitive) {
                scan.primitive = true;
                return ++scan.nodes <= limits.maximumNodes;
            }
            return true;
        }
    }  // namespace

    /** @copydoc FrameWithinBounds */
    bool FrameWithinBounds(const std::string_view frame, const McpSessionLimits &limits) noexcept {
        if (frame.size() > limits.maximumFrameBytes)
            return false;
        FrameScan scan;
        for (const unsigned char value : frame) {
            if (scan.quoted) {
                if (!VisitQuotedByte(value, limits, scan))
                    return false;
                continue;
            }
            if (!VisitUnquotedByte(value, limits, scan))
                return false;
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
