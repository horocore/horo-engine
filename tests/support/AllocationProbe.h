#pragma once

#include <cstddef>

namespace Horo::Tests::AllocationProbe {
    /** @brief Owner-thread allocation observer; must not allocate or throw. */
    using AllocationObserver = void (*)(std::size_t byteCount, void *context) noexcept;

    /** @brief Scoped synchronous allocator seam, isolated from allocations on other threads. */
    class ScopedObserver final {
    public:
        /** @brief Observe requests until scope exit; context must outlive this scope. */
        ScopedObserver(AllocationObserver observer, void *context) noexcept;
        ~ScopedObserver();
        ScopedObserver(const ScopedObserver &) = delete;
        ScopedObserver &operator=(const ScopedObserver &) = delete;

    private:
        AllocationObserver previousObserver_{};
        void *previousContext_{};
    };

    /** @brief Ordinary/aligned C++ allocation requests in one owner-thread measurement window; not peak resident memory. */
    struct Measurement final {
        std::size_t requests{};
        std::size_t requestedBytes{};
        std::size_t largestRequest{};
    };

    /** @brief Non-allocating scoped observer; excludes other threads and direct C/native allocations. */
    class ScopedMeasurement final {
    public:
        ScopedMeasurement() noexcept;
        ~ScopedMeasurement();
        ScopedMeasurement(const ScopedMeasurement &) = delete;
        ScopedMeasurement &operator=(const ScopedMeasurement &) = delete;
        /** @brief Copies observed requests without allocating. @return Count, cumulative bytes and largest request. */
        [[nodiscard]] Measurement Snapshot() const noexcept;

    private:
        Measurement measurement_{};
        Measurement *previous_{};
    };

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
