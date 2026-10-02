#pragma once

#include <cstddef>

namespace Horo::Tests::AllocationProbe {
    /** @brief One-shot allocation failure scope, disabled again after the injected failure or scope exit. */
    class ScopedFailure final {
    public:
        /** @brief Fails after the requested number of successful allocations. */
        explicit ScopedFailure(std::size_t successfulAllocationsBeforeFailure = 0) noexcept;
        ~ScopedFailure();
        ScopedFailure(const ScopedFailure &) = delete;
        ScopedFailure &operator=(const ScopedFailure &) = delete;
    };

    /** @brief Returns the allocation count observed by the test executable. */
    [[nodiscard]] std::size_t Count() noexcept;
    /** @brief Returns actual nonnull deallocations observed by the test executable. */
    [[nodiscard]] std::size_t FreeCount() noexcept;
}  // namespace Horo::Tests::AllocationProbe
