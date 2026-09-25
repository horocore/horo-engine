#pragma once

/**
 * @file AssertionFormatting.h
 * @brief Bounded output storage for emergency assertion text.
 */

#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

namespace Horo::AssertionPolicy::Detail {
    inline constexpr std::size_t FailureBufferBytes = 1024;
    inline constexpr std::size_t MaximumFileBytes = 192;
    inline constexpr std::size_t MaximumFunctionBytes = 160;
    inline constexpr std::size_t MaximumExpressionBytes = 256;
    inline constexpr std::size_t MaximumMessageBytes = 256;
    static_assert(19 + MaximumFileBytes + 10 + MaximumFunctionBytes + MaximumExpressionBytes + MaximumMessageBytes + 13 <
                  FailureBufferBytes);

    /**
     * @brief Formats null-terminated context without allowing an exception to escape the fatal boundary.
     * @param kind Assertion or invariant label.
     * @param expression Source expression, clipped to MaximumExpressionBytes.
     * @param message Optional detail, clipped to MaximumMessageBytes.
     * @param file Source path, clipped to MaximumFileBytes.
     * @param line Source line number.
     * @param function Source function, clipped to MaximumFunctionBytes.
     * @return Fixed-size character storage containing the formatted text or an emergency fallback.
     */
    [[nodiscard]] inline std::array<char, FailureBufferBytes> FormatFailure(const FailureKind kind, const std::string_view expression,
                                                                            const std::string_view message, const std::string_view file,
                                                                            const std::uint_least32_t line,
                                                                            const std::string_view function) noexcept {
        std::array<char, FailureBufferBytes> formatted{};
        const std::string_view kindName = kind == FailureKind::Invariant ? "Invariant violation" : "Assertion failed";
        try {
            const auto result = std::format_to_n(formatted.data(), formatted.size() - 1, "{} at {}:{} in {}: {}{}{}", kindName,
                                                 file.substr(0, MaximumFileBytes), line, function.substr(0, MaximumFunctionBytes),
                                                 expression.substr(0, MaximumExpressionBytes), message.empty() ? "" : ": ",
                                                 message.substr(0, MaximumMessageBytes));
            *result.out = '\0';
        } catch (...) {  // NOSONAR(cpp:S1181): the fatal boundary must still emit a record if formatting throws.
            constexpr std::string_view fallback = "Assertion formatting failed";
            std::ranges::copy(fallback, formatted.begin());
        }
        return formatted;
    }
}  // namespace Horo::AssertionPolicy::Detail
