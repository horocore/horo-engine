#pragma once

#include <algorithm>
#include <string_view>

namespace Horo::Debug::Detail {
    /** @brief Clips already validated UTF-8 without splitting a scalar sequence.
     * @param text Canonical UTF-8 bytes. @param maximumBytes Finite byte budget. @return Borrowed complete-scalar prefix.
     */
    [[nodiscard]] inline std::string_view ConsoleTextPrefix(std::string_view text, const std::size_t maximumBytes) noexcept {
        auto end = std::min(text.size(), maximumBytes);
        if (end == text.size())
            return text;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
            --end;
        return text.substr(0, end);
    }
}  // namespace Horo::Debug::Detail
