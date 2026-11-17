#pragma once

/**
 * @file UiCpuWorkStatistics.h
 * @brief Opt-in owner-thread CPU timing evidence for Runtime UI work.
 */

#include <cstdint>

namespace Horo::Runtime::Ui {
    /** @brief Value-only statistics for the current owner lifetime, including failed attempts. */
    struct UiCpuWorkStatistics final {
        bool enabled{};                     /**< False when the owner skips clock reads and counter updates. */
        std::uint64_t attempts{};           /**< Saturating count of measured work attempts. */
        std::uint64_t succeeded{};          /**< Saturating count of attempts that produced their requested result. */
        std::uint64_t elapsedNanoseconds{}; /**< Saturating total monotonic time, including failed attempts. */
        std::uint64_t lastNanoseconds{};    /**< Duration of the latest attempt. */
    };
}  // namespace Horo::Runtime::Ui
