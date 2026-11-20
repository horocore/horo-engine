#pragma once

/** @file EditorNumberParsing.h @brief Exact locale-independent parsing of retained editor numeric fields. */

#include <charconv>
#include <cmath>
#include <concepts>
#include <string_view>

#ifdef __APPLE__
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <locale.h>
#include <xlocale.h>
#endif

namespace Horo::Editor {
#ifdef __APPLE__
    namespace NumberParsingDetail {
        /** @brief Owns the immutable C numeric locale used by libc before macOS has floating-point from_chars. */
        struct NumericLocale final {
            locale_t value{newlocale(LC_NUMERIC_MASK, "C", nullptr)};

            ~NumericLocale() {
                if (value != nullptr)
                    freelocale(value);
            }
        };

        /** @brief Parses the same finite decimal grammar as from_chars without depending on a recent SDK symbol. */
        template <std::floating_point T> bool Parse(const std::string_view text, T &value) noexcept {
            // All current editor numeric controls admit at most 128 characters.
            std::array<char, 129> terminated{};
            if (text.size() >= terminated.size() || text.front() == '+' ||
                text.find_first_not_of("0123456789-+.eE") != std::string_view::npos)
                return false;
            std::ranges::copy(text, terminated.begin());

            static const NumericLocale locale;
            if (locale.value == nullptr)
                return false;
            char *end{};
            const auto savedError = errno;
            errno = 0;
            const auto candidate = [&] {
                if constexpr (std::same_as<T, float>)
                    return strtof_l(terminated.data(), &end, locale.value);
                else if constexpr (std::same_as<T, double>)
                    return strtod_l(terminated.data(), &end, locale.value);
                else
                    return strtold_l(terminated.data(), &end, locale.value);
            }();
            const auto conversionError = errno;
            errno = savedError;
            // libc may report underflow even when the rounded finite subnormal is representable.
            const bool representable = conversionError == 0 || (conversionError == ERANGE && candidate != 0 &&
                                                                std::abs(candidate) <= std::numeric_limits<T>::min());
            if (!representable || !std::isfinite(candidate) || end != terminated.data() + text.size())
                return false;
            value = candidate;
            return true;
        }
    }  // namespace NumberParsingDetail
#endif

    /** @brief Parses complete editor numeric text without changing the destination on invalid input.
     * @param text Retained decimal text without its terminator.
     * @param value Destination replaced only by a finite value of the exact requested type.
     * @return Whether the complete input was accepted.
     */
    template <typename T> [[nodiscard]] bool ParseEditorNumber(const std::string_view text, T &value) noexcept {
        if (text.empty())
            return false;
#ifdef __APPLE__
        if constexpr (std::floating_point<T>) {
            return NumberParsingDetail::Parse(text, value);
        } else
#endif
        {
            T candidate{};
            const auto *end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, candidate);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                return false;
            if constexpr (std::floating_point<T>)
                if (!std::isfinite(candidate))
                    return false;
            value = candidate;
            return true;
        }
    }
}  // namespace Horo::Editor
