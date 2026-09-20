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
    class AllocationMeter final {
    public:
        [[nodiscard]] static void *Acquire(const std::size_t byteCount) {
            count_.fetch_add(1, std::memory_order_relaxed);
            std::size_t remaining = failureCountdown_.load(std::memory_order_relaxed);
            while (remaining != DisabledFailureCountdown) {
                if (remaining == 0) {
                    if (failureCountdown_.compare_exchange_weak(remaining, DisabledFailureCountdown, std::memory_order_relaxed))
                        throw std::bad_alloc{};
                } else if (failureCountdown_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            void *const storage = std::malloc(std::max(byteCount, std::size_t{1}));
            if (storage == nullptr)
                throw std::bad_alloc{};
            return storage;
        }

        static void Release(void *const storage) noexcept {
            std::free(storage);
        }

        [[nodiscard]] static void *AcquireAligned(const std::size_t byteCount, const std::size_t alignment) {
            count_.fetch_add(1, std::memory_order_relaxed);
            std::size_t remaining = failureCountdown_.load(std::memory_order_relaxed);
            while (remaining != DisabledFailureCountdown) {
                if (remaining == 0) {
                    if (failureCountdown_.compare_exchange_weak(remaining, DisabledFailureCountdown, std::memory_order_relaxed))
                        throw std::bad_alloc{};
                } else if (failureCountdown_.compare_exchange_weak(remaining, remaining - 1, std::memory_order_relaxed)) {
                    break;
                }
            }
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
#ifdef _WIN32
            _aligned_free(storage);
#else
            std::free(storage);
#endif
        }

        [[nodiscard]] static std::size_t Count() noexcept {
            return count_.load(std::memory_order_relaxed);
        }

        static void FailAfter(const std::size_t successfulAllocations) noexcept {
            failureCountdown_.store(successfulAllocations, std::memory_order_relaxed);
        }

        static void DisableFailures() noexcept {
            failureCountdown_.store(DisabledFailureCountdown, std::memory_order_relaxed);
        }

    private:
        static constexpr std::size_t DisabledFailureCountdown = std::numeric_limits<std::size_t>::max();
        static inline std::atomic<std::size_t> count_{};
        static inline std::atomic<std::size_t> failureCountdown_{DisabledFailureCountdown};
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
    ScopedFailure::ScopedFailure(const std::size_t successfulAllocationsBeforeFailure) noexcept {
        AllocationMeter::FailAfter(successfulAllocationsBeforeFailure);
    }

    ScopedFailure::~ScopedFailure() {
        AllocationMeter::DisableFailures();
    }

    std::size_t Count() noexcept {
        return AllocationMeter::Count();
    }
}  // namespace Horo::Tests::AllocationProbe
