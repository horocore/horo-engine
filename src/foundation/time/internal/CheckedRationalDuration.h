#pragma once

/** @file CheckedRationalDuration.h
 * @brief Non-installed exact checked arithmetic shared by actual host and UI duration owners.
 */
#include <cstdint>
#include <limits>
#include <numeric>

namespace Horo::Foundation::TimeInternal {
    /** @brief Arithmetic failure only; owning subsystems supply their own typed domain errors. */
    enum class ArithmeticError : std::uint8_t {
        None,
        Invalid,
        Overflow
    };

    /** @brief Reduced positive-denominator fraction of one nanosecond. */
    struct Fraction final {
        std::uint32_t numerator{};
        std::uint32_t denominator{1};
    };

    /** @brief Complete value candidate; no caller storage changes on failure. */
    struct ScaledDuration final {
        std::int64_t nanoseconds{};
        Fraction remainder;
        ArithmeticError error{};
    };

    /** @brief Qualifies an exact fractional nanosecond without silently losing malformed time. */
    [[nodiscard]] inline bool ValidFraction(const Fraction fraction) noexcept {
        return fraction.denominator != 0 && fraction.numerator < fraction.denominator;
    }

    /** @brief Reduces an already validated fraction, including canonical zero. */
    [[nodiscard]] inline Fraction Reduce(const Fraction fraction) noexcept {
        const auto divisor = std::gcd(fraction.numerator, fraction.denominator);
        return {fraction.numerator / divisor, fraction.denominator / divisor};
    }

    /** @brief Combines finite exact fractions and checks integral carry before returning a candidate. */
    [[nodiscard]] inline ScaledDuration Combine(const std::int64_t whole, Fraction current, Fraction previous) noexcept {
        if (whole < 0 || !ValidFraction(current) || !ValidFraction(previous))
            return {.error = ArithmeticError::Invalid};
        current = Reduce(current);
        previous = Reduce(previous);
        const auto divisor = std::gcd(current.denominator, previous.denominator);
        const auto common = static_cast<std::uint64_t>(current.denominator / divisor) * previous.denominator;
        if (common > std::numeric_limits<std::uint32_t>::max())
            return {.error = ArithmeticError::Overflow};
        const auto fraction = static_cast<std::uint64_t>(current.numerator) * (common / current.denominator) +
                              static_cast<std::uint64_t>(previous.numerator) * (common / previous.denominator);
        const auto carry = static_cast<std::int64_t>(fraction / common);
        if (whole > std::numeric_limits<std::int64_t>::max() - carry)
            return {.error = ArithmeticError::Overflow};
        const auto residue = static_cast<std::uint32_t>(fraction % common);
        return {whole + carry, Reduce({residue, static_cast<std::uint32_t>(common)}), ArithmeticError::None};
    }

    /** @brief Scales integer nanoseconds while retaining exact carry across rational rate changes. */
    [[nodiscard]] inline ScaledDuration Scale(const std::int64_t nanoseconds, const std::uint32_t numerator,
                                              const std::uint32_t denominator, const Fraction remainder) noexcept {
        if (nanoseconds < 0 || denominator == 0 || !ValidFraction(remainder))
            return {.error = ArithmeticError::Invalid};
        const auto input = static_cast<std::uint64_t>(nanoseconds);
        constexpr auto Maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const auto quotient = input / denominator;
        if (numerator != 0 && quotient > Maximum / numerator)
            return {.error = ArithmeticError::Overflow};
        const auto tail = (input % denominator) * numerator;
        const auto extraWhole = tail / denominator;
        const auto scaledWhole = quotient * numerator;
        if (extraWhole > Maximum - scaledWhole)
            return {.error = ArithmeticError::Overflow};
        return Combine(static_cast<std::int64_t>(scaledWhole + extraWhole), {static_cast<std::uint32_t>(tail % denominator), denominator},
                       remainder);
    }
}  // namespace Horo::Foundation::TimeInternal
