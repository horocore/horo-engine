#pragma once

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string_view>

namespace Horo::Render::ShaderValidationDetail {
    [[nodiscard]] inline bool IsValidIdentity(const std::string_view value, const std::size_t maximumBytes,
                                              const bool allowPlus = false) noexcept {
        if (value.empty() || value.size() > maximumBytes)
            return false;
        return std::ranges::all_of(value, [allowPlus](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            const bool alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            const bool digit = byte >= '0' && byte <= '9';
            return alpha || digit || byte == '.' || byte == '_' || byte == '-' || byte == '/' || (allowPlus && byte == '+');
        });
    }
}  // namespace Horo::Render::ShaderValidationDetail
