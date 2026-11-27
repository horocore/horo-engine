#pragma once

#include "Horo/Runtime/Ui/UiCpuWorkStatistics.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @brief Records one optional monotonic sample without callbacks or allocation. */
    class UiCpuWorkProbe final {
    public:
        explicit UiCpuWorkProbe(UiCpuWorkStatistics &statistics) noexcept : statistics_(statistics.enabled ? &statistics : nullptr) {
            if (statistics_)
                started_ = Clock::now();
        }

        UiCpuWorkProbe(const UiCpuWorkProbe &) = delete;
        UiCpuWorkProbe &operator=(const UiCpuWorkProbe &) = delete;

        ~UiCpuWorkProbe() noexcept {
            if (!statistics_)
                return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started_).count();
            statistics_->lastNanoseconds = static_cast<std::uint64_t>(elapsed);
            Add(statistics_->elapsedNanoseconds, statistics_->lastNanoseconds);
            Add(statistics_->attempts, 1);
            Add(statistics_->succeeded, succeeded_ ? 1 : 0);
        }

        /** @brief Records that the scope produced its requested result. */
        void Succeed() noexcept {
            succeeded_ = true;
        }

    private:
        using Clock = std::chrono::steady_clock;

        static void Add(std::uint64_t &total, const std::uint64_t increment) noexcept {
            total += std::min(increment, std::numeric_limits<std::uint64_t>::max() - total);
        }

        UiCpuWorkStatistics *statistics_{};
        Clock::time_point started_;
        bool succeeded_{};
    };
}  // namespace Horo::Runtime::Ui
