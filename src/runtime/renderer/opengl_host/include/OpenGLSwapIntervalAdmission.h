#pragma once

/** @file OpenGLSwapIntervalAdmission.h
 * @brief Non-installed, allocation-free host admission with no retained callbacks or native handles.
 */

namespace Horo::Render::Detail {
    /** @brief Exact interval admission evidence, not a fallback or hardware-support claim. */
    enum class SwapIntervalAdmission {
        Exact,
        QueryFailed,
        SetFailed,
        Mismatch
    };

    /** @brief Queries first so an already active exact interval needs no swap-control setter.
     * @param interval Explicitly required interval.
     * @param query Native readback; false never proves the initialized output value.
     * @param set Native setter, called only when the verified active interval differs.
     * @return Exact readback or the failed operation; never accepts a mismatched interval.
     */
    template <typename Query, typename Set>
    [[nodiscard]] SwapIntervalAdmission AdmitExactSwapInterval(const int interval, Query query, Set set) {
        int actual = 0;
        if (!query(&actual))
            return SwapIntervalAdmission::QueryFailed;
        if (actual == interval)
            return SwapIntervalAdmission::Exact;
        if (!set(interval))
            return SwapIntervalAdmission::SetFailed;
        if (!query(&actual))
            return SwapIntervalAdmission::QueryFailed;
        return actual == interval ? SwapIntervalAdmission::Exact : SwapIntervalAdmission::Mismatch;
    }
}  // namespace Horo::Render::Detail
