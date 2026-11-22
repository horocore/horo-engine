#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace Horo::Diagnostics::Detail {
    /** @brief Classifies credential-bearing keys consistently at log admission and bundle export. */
    [[nodiscard]] inline bool IsSensitiveKey(const std::string_view key) {
        if (key.empty())
            return false;
        std::string normalized{key};
        std::erase_if(normalized, [](const char character) {
            return character == '_' || character == '-' || character == '.';
        });
        std::ranges::transform(normalized, normalized.begin(), [](const char character) {
            return character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character;
        });
        static constexpr std::array<std::string_view, 8> fragments{"apikey",   "authorization", "cookie", "credential",
                                                                   "password", "privatekey",    "secret", "token"};
        return std::ranges::any_of(fragments, [&normalized](const std::string_view fragment) {
            return normalized.find(fragment) != std::string::npos;
        });
    }
}  // namespace Horo::Diagnostics::Detail
