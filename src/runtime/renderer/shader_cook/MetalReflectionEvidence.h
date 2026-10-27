#pragma once

/** @file MetalReflectionEvidence.h
 * @brief Pure bounded lexical evidence shared by the closed Metal cook interfaces.
 */
#include <charconv>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Horo::Render::MetalReflectionCookDetail {
    struct InvalidEvidence {};

    /** @brief Skips one comment, whitespace byte or quoted literal without exposing binding evidence. */
    inline bool SkipNonEvidence(const std::string_view text, std::size_t &cursor) {
        const auto ch = text[cursor];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            ++cursor;
            return true;
        }
        if (text.substr(cursor, 2) == "//") {
            const auto end = text.find('\n', cursor + 2);
            cursor = end == std::string_view::npos ? text.size() : end;
            return true;
        }
        if (text.substr(cursor, 2) == "/*") {
            const auto end = text.find("*/", cursor + 2);
            if (end == std::string_view::npos)
                throw InvalidEvidence{};
            cursor = end + 2;
            return true;
        }
        if (ch != '"' && ch != '\'')
            return false;
        ++cursor;
        while (cursor < text.size() && text[cursor] != ch) {
            if (text[cursor] == '\\')
                ++cursor;
            ++cursor;
        }
        if (cursor >= text.size())
            throw InvalidEvidence{};
        ++cursor;
        return true;
    }

    /** @brief Recognizes only the generated ASCII identifier and numeric token alphabet. */
    inline bool IsWord(const char value) noexcept {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '_';
    }

    /** @brief Tokenizes bounded generated MSL while excluding comments and string literals from binding evidence. */
    inline std::vector<std::string_view> Tokens(const std::string_view text) {
        if (text.empty() || text.size() > 1024U * 1024U)
            throw InvalidEvidence{};
        std::vector<std::string_view> result;
        for (std::size_t cursor = 0; cursor < text.size();) {
            const auto start = cursor;
            if (SkipNonEvidence(text, cursor))
                continue;
            if (IsWord(text[cursor])) {
                while (cursor < text.size() && IsWord(text[cursor]))
                    ++cursor;
            } else {
                ++cursor;
            }
            if (result.size() == 131072)
                throw InvalidEvidence{};
            result.push_back(text.substr(start, cursor - start));
        }
        return result;
    }

    /** @brief Parses the complete bounded native resource attribute after its recognized kind. */
    inline std::uint32_t AttributeIndex(const std::vector<std::string_view> &tokens, const std::size_t index) {
        if (tokens[index + 3] != "(" || tokens[index + 5] != ")" || tokens[index + 6] != "]" || tokens[index + 7] != "]")
            throw InvalidEvidence{};
        std::uint32_t value{};
        const auto number = tokens[index + 4];
        if (const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || value > 30)
            throw InvalidEvidence{};
        return value;
    }

}  // namespace Horo::Render::MetalReflectionCookDetail
