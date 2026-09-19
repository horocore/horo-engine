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
            ConsumeFailureBudget();
            void *const storage = std::malloc(std::max(byteCount, std::size_t{1}));
            if (storage == nullptr)
                throw std::bad_alloc{};
            return storage;
        }

        static void Release(void *const storage) noexcept {
            std::free(storage);
        }

        [[nodiscard]] static void *AcquireAligned(const std::size_t byteCount, const std::size_t alignment) {
            ConsumeFailureBudget();
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

        [[nodiscard]] static void *TryAcquire(const std::size_t byteCount) noexcept {
            try {
                return Acquire(byteCount);
            } catch (...) {
                return nullptr;
            }
        }

        [[nodiscard]] static void *TryAcquireAligned(const std::size_t byteCount, const std::size_t alignment) noexcept {
            try {
                return AcquireAligned(byteCount, alignment);
            } catch (...) {
                return nullptr;
            }
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
            failureCountdown_ = successfulAllocations;
        }

        static void DisableFailures() noexcept {
            failureCountdown_ = DisabledFailureCountdown;
        }

    private:
        static constexpr std::size_t DisabledFailureCountdown = std::numeric_limits<std::size_t>::max();
        static inline std::atomic<std::size_t> count_{};
        // Failure injection is scoped to the calling test thread. Other runtime or
        // CRT threads must continue allocating while a test exercises one operation.
        static inline thread_local std::size_t failureCountdown_ = DisabledFailureCountdown;

        static void ConsumeFailureBudget() {
            count_.fetch_add(1, std::memory_order_relaxed);
            if (failureCountdown_ == DisabledFailureCountdown)
                return;
            if (failureCountdown_ == 0) {
                failureCountdown_ = DisabledFailureCountdown;
                throw std::bad_alloc{};
            }
            --failureCountdown_;
        }
    };
}  // namespace

void *operator new(const std::size_t size) {
    return AllocationMeter::Acquire(size);
}

void *operator new[](const std::size_t size) {
    return AllocationMeter::Acquire(size);
}

void *operator new(const std::size_t size, const std::nothrow_t &) noexcept {
    return AllocationMeter::TryAcquire(size);
}

void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept {
    return AllocationMeter::TryAcquire(size);
}

void *operator new(const std::size_t size, const std::align_val_t alignment) {
    return AllocationMeter::AcquireAligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](const std::size_t size, const std::align_val_t alignment) {
    return AllocationMeter::AcquireAligned(size, static_cast<std::size_t>(alignment));
}

void *operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    return AllocationMeter::TryAcquireAligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](const std::size_t size, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    return AllocationMeter::TryAcquireAligned(size, static_cast<std::size_t>(alignment));
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

void operator delete(void *memory, const std::nothrow_t &) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete(void *memory, std::size_t, const std::nothrow_t &) noexcept {
    AllocationMeter::Release(memory);
}

void operator delete[](void *memory, std::size_t, const std::nothrow_t &) noexcept {
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

void operator delete(void *memory, const std::align_val_t, const std::nothrow_t &) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete[](void *memory, const std::align_val_t, const std::nothrow_t &) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete(void *memory, std::size_t, const std::align_val_t, const std::nothrow_t &) noexcept {
    AllocationMeter::ReleaseAligned(memory);
}

void operator delete[](void *memory, std::size_t, const std::align_val_t, const std::nothrow_t &) noexcept {
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
