#pragma once

/** @file @brief Capacity-qualified literal copying for private native OpenXR ABI fields. */

#include <algorithm>
#include <span>

namespace Horo::XR::OpenXRInternal {
    /**
     * @brief Copies a literal including its NUL into unchanged fixed-capacity native storage.
     * @param destination Native ABI character array; remaining bytes are untouched.
     * @param literal Compile-time-sized literal including the terminating NUL.
     * @pre The literal must fit the destination, enforced at compile time.
     */
    template <std::size_t Capacity, std::size_t Size>
    constexpr void CopyNativeLiteral(char (&destination)[Capacity], const char (&literal)[Size]) noexcept {
        static_assert(Size <= Capacity, "OpenXR literal must fit the ABI field including its terminator");
        std::ranges::copy(std::span{literal}, std::span{destination}.first(Size).begin());
    }
}  // namespace Horo::XR::OpenXRInternal
