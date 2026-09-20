#pragma once

/**
 * @file MathUtils.h
 * @brief Exact unsigned arithmetic helpers shared by progress contracts.
 */

#include <cstdint>

namespace Horo::Foundation::Math {
    /** @brief The exact 128-bit product of two unsigned 64-bit values. */
    struct WideProduct final {
        std::uint64_t high{}; /**< Most-significant 64 bits of the product. */
        std::uint64_t low{};  /**< Least-significant 64 bits of the product. */
    };

    /**
     * @brief Multiplies two unsigned 64-bit values without overflowing.
     * @param left First factor.
     * @param right Second factor.
     * @return The exact product split into high and low 64-bit words.
     */
    [[nodiscard]] constexpr WideProduct MultiplyWide(const std::uint64_t left, const std::uint64_t right) noexcept {
        constexpr std::uint64_t lowerMask = 0xffffffffULL;
        const std::uint64_t leftLow = left & lowerMask;
        const std::uint64_t leftHigh = left >> 32U;
        const std::uint64_t rightLow = right & lowerMask;
        const std::uint64_t rightHigh = right >> 32U;
        const std::uint64_t lowProduct = leftLow * rightLow;
        const std::uint64_t firstCross = leftHigh * rightLow + (lowProduct >> 32U);
        const std::uint64_t secondCross = leftLow * rightHigh + (firstCross & lowerMask);
        return {.high = leftHigh * rightHigh + (firstCross >> 32U) + (secondCross >> 32U),
                .low = (secondCross << 32U) + (lowProduct & lowerMask)};
    }

    /**
     * @brief Compares two exact unsigned progress ratios without floating-point rounding.
     * @param currentCompleted Completed units of the current progress value.
     * @param currentTotal Total units of the current progress value.
     * @param nextCompleted Completed units of the candidate progress value.
     * @param nextTotal Total units of the candidate progress value.
     * @return True when the candidate ratio is lower than the current ratio.
     */
    [[nodiscard]] constexpr bool IsProgressRegression(const std::uint64_t currentCompleted, const std::uint64_t currentTotal,
                                                      const std::uint64_t nextCompleted, const std::uint64_t nextTotal) noexcept {
        const WideProduct nextProduct = MultiplyWide(nextCompleted, currentTotal);
        const WideProduct currentProduct = MultiplyWide(currentCompleted, nextTotal);
        return nextProduct.high < currentProduct.high || (nextProduct.high == currentProduct.high && nextProduct.low < currentProduct.low);
    }
}  // namespace Horo::Foundation::Math
