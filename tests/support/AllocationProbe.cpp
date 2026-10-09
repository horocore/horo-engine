#include "AllocationProbe.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
    // Scoped stack-owned observation on the calling thread; fixture/framework allocations stay outside its window.
    thread_local Horo::Tests::AllocationProbe::Measurement *activeMeasurement{};
    thread_local Horo::Tests::AllocationProbe::AllocationObserver activeObserver{};
    thread_local void *observerContext{};

    class AllocationMeter final {
    public:
        [[nodiscard]] static void *Acquire(const std::size_t byteCount) {
            RecordAllocation(byteCount);
            void *const storage = std::malloc(std::max(byteCount, std::size_t{1}));
            if (storage == nullptr)
                throw std::bad_alloc{};
            return storage;
        }

        static void Release(void *const storage) noexcept {
            if (storage != nullptr)
                freeCount_.fetch_add(1, std::memory_order_relaxed);
            std::free(storage);
        }

        [[nodiscard]] static void *AcquireAligned(const std::size_t byteCount, const std::size_t alignment) {
            RecordAllocation(byteCount);
#ifdef _WIN32
            void *const storage = _aligned_malloc(std::max(byteCount, std::size_t{1}), alignment);
#else
            void *storage = nullptr;
            if (posix_memalign(&storage, alignment, std::max(byteCount, std::size_t{1})) != 0)
                storage = nullptr;
#endif
            if (storage == nullptr)
                throw std::bad_alloc{};
            return storage;
        }

        static void ReleaseAligned(void *const storage) noexcept {
            if (storage != nullptr)
                freeCount_.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
            _aligned_free(storage);
#else
            std::free(storage);
#endif
        }

        [[nodiscard]] static std::size_t Count() noexcept {
            return count_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] static std::size_t FreeCount() noexcept {
            return freeCount_.load(std::memory_order_relaxed);
        }

        static void FailAfter(const std::size_t successfulAllocations, Horo::Tests::AllocationProbe::FailureObserver observer) noexcept {
            failureObserver_.store(observer, std::memory_order_relaxed);
            failureCountdown_.store(successfulAllocations, std::memory_order_relaxed);
        }

        static void DisableFailures() noexcept {
            failureCountdown_.store(DisabledFailureCountdown, std::memory_order_relaxed);
            failureObserver_.store(nullptr, std::memory_order_relaxed);
        }

    private:
        /** @brief Disable the one-shot failure before notifying or throwing, so exception cleanup can allocate. */
        static void RecordAllocation(const std::size_t byteCount) {
            if (activeObserver != nullptr)
                activeObserver(byteCount, observerContext);
            if (activeMeasurement != nullptr) {
                ++activeMeasurement->requests;
                const auto remaining = std::numeric_limits<std::size_t>::max() - activeMeasurement->requestedBytes;
                activeMeasurement->requestedBytes += std::min(byteCount, remaining);
                activeMeasurement->largestRequest = std::max(activeMeasurement->largestRequest, byteCount);
            }
            count_.fetch_add(1, std::memory_order_relaxed);
            std::size_t remaining = failureCountdown_.load(std::memory_order_relaxed);
            while (remaining != DisabledFailureCountdown) {
                if (remaining == 0) {
                    if (failureCountdown_.compare_exchange_weak(remaining, DisabledFailureCountdown, std::memory_order_relaxed)) {
                        if (const auto observer = failureObserver_.load(std::memory_order_relaxed); observer != nullptr)
                            observer(byteCount);
                        throw std::bad_alloc{};
                    }
                } else if (failureCountdown_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        static constexpr std::size_t DisabledFailureCountdown = std::numeric_limits<std::size_t>::max();
        static inline std::atomic<std::size_t> count_{};
        static inline std::atomic<std::size_t> freeCount_{};
        static inline std::atomic<std::size_t> failureCountdown_{DisabledFailureCountdown};
        static inline std::atomic<Horo::Tests::AllocationProbe::FailureObserver> failureObserver_{};
    };
}  // namespace

void *operator new(const std::size_t size) {
    return AllocationMeter::Acquire(size);
}

void *operator new[](const std::size_t size) {
    return AllocationMeter::Acquire(size);
}

void *operator new(const std::size_t size, const std::align_val_t alignment) {
    return AllocationMeter::AcquireAligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](const std::size_t size, const std::align_val_t alignment) {
    return AllocationMeter::AcquireAligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *memory) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete[](void *memory) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete(void *memory, const std::align_val_t) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete[](void *memory, const std::align_val_t) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete(void *memory, std::size_t, const std::align_val_t) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete[](void *memory, std::size_t, const std::align_val_t) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

namespace Horo::Tests::AllocationProbe {
    /** @copydoc ScopedObserver::ScopedObserver */
    ScopedObserver::ScopedObserver(const AllocationObserver observer, void *context) noexcept
        : previousObserver_(activeObserver), previousContext_(observerContext) {
        activeObserver = observer;
        observerContext = context;
    }

    ScopedObserver::~ScopedObserver() {
        activeObserver = previousObserver_;
        observerContext = previousContext_;
    }

    ScopedMeasurement::ScopedMeasurement() noexcept : previous_(activeMeasurement) {
        activeMeasurement = &measurement_;
    }

    ScopedMeasurement::~ScopedMeasurement() {
        activeMeasurement = previous_;
    }

    Measurement ScopedMeasurement::Snapshot() const noexcept {
        return measurement_;
    }

    /** @copydoc ScopedFailure::ScopedFailure */
    ScopedFailure::ScopedFailure(const std::size_t successfulAllocationsBeforeFailure, FailureObserver observer) noexcept {
        AllocationMeter::FailAfter(successfulAllocationsBeforeFailure, observer);
    }

    ScopedFailure::~ScopedFailure() {
        AllocationMeter::DisableFailures();
    }

    std::size_t Count() noexcept {
        return AllocationMeter::Count();
    }

    std::size_t FreeCount() noexcept {
        return AllocationMeter::FreeCount();
    }
}  // namespace Horo::Tests::AllocationProbe
