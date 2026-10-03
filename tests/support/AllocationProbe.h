#pragma once

#include <cstddef>

namespace Horo::Tests::AllocationProbe {
    /** @brief Non-allocating observer called immediately before an injected failure is thrown. */
    using FailureObserver = void (*)(std::size_t byteCount) noexcept;

    /** @brief One-shot allocation failure scope, disabled again after the injected failure or scope exit. */
    class ScopedFailure final {
    public:
        /** @brief Fail once after the requested number of successful allocations.
         * @param successfulAllocationsBeforeFailure Number of ordinary/aligned requests allowed before failure.
         * @param observer Optional non-allocating observer; failures are disabled before it is invoked.
         */
        explicit ScopedFailure(std::size_t successfulAllocationsBeforeFailure = 0, FailureObserver observer = nullptr) noexcept;
        ~ScopedFailure();
        ScopedFailure(const ScopedFailure &) = delete;
        ScopedFailure &operator=(const ScopedFailure &) = delete;
    };

    /** @brief Returns the allocation count observed by the test executable. */
    [[nodiscard]] std::size_t Count() noexcept;
    /** @brief Returns actual nonnull deallocations observed by the test executable. */
    [[nodiscard]] std::size_t FreeCount() noexcept;
}  // namespace Horo::Tests::AllocationProbe
