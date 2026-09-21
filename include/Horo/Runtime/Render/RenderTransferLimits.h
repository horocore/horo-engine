#pragma once

/**
 * @file RenderTransferLimits.h
 * @brief Shared validation for bounded render transfer limits.
 */

#include <cstddef>
#include <cstdint>

namespace Horo::Render::detail {
    /**
     * @brief Validates the bounds shared by bounded render transfer queues.
     * @param maximumPendingBytes Maximum bytes that may remain in flight.
     * @param maximumRequestBytes Maximum bytes admitted by one request.
     * @param maximumAlignment Maximum staging alignment.
     * @param maximumRequests Maximum retained request records.
     * @return True when every bound is finite, non-zero, and mutually consistent.
     */
    [[nodiscard]] constexpr bool IsValidBoundedRenderQueueLimits(const std::size_t maximumPendingBytes,
                                                                 const std::size_t maximumRequestBytes, const std::size_t maximumAlignment,
                                                                 const std::uint32_t maximumRequests) noexcept {
        return maximumPendingBytes > 0 && maximumRequestBytes > 0 && maximumRequestBytes <= maximumPendingBytes && maximumAlignment > 0 &&
               (maximumAlignment & (maximumAlignment - 1U)) == 0 && maximumRequests > 0;
    }
}  // namespace Horo::Render::detail
