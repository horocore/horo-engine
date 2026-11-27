#include "AllocationProbe.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#if defined(_WIN32) || defined(__linux__)
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif

namespace {
    // Scoped stack-owned observation on the calling thread; fixture/framework allocations stay outside its window.
    thread_local Horo::Tests::AllocationProbe::Measurement *activeMeasurement{};
    thread_local Horo::Tests::AllocationProbe::AllocationObserver activeObserver{};
    thread_local void *observerContext{};
    using LiveProbe = Horo::Tests::AllocationProbe::ScopedLiveMeasurement;
    // The lock protects the active scope pointer and its caller-owned records across allocator threads.
    constinit std::atomic<LiveProbe *> activeLiveProbe{};
    std::mutex liveProbeMutex;

    /** @brief Reads usable capacity for an allocation made by the matching CRT allocator. */
    std::size_t UsableAllocationBytes(void *const storage, [[maybe_unused]] const std::size_t alignment) noexcept {
#ifdef _WIN32
        return alignment == 0 ? _msize(storage) : _aligned_msize(storage, alignment, 0);
#elif defined(__linux__)
        return malloc_usable_size(storage);
#elif defined(__APPLE__)
        return malloc_size(storage);
#else
        static_cast<void>(storage);
        return 0;
#endif
    }

    class AllocationMeter final {
    public:
        [[nodiscard]] static void *Acquire(const std::size_t byteCount) {
            RecordAllocation(byteCount);
            void *const storage = std::malloc(std::max(byteCount, std::size_t{1}));
            if (storage == nullptr)
                throw std::bad_alloc{};
            LiveProbe::Acquired(storage, byteCount);
            return storage;
        }

        static void Release(void *const storage) noexcept {
            if (storage != nullptr) {
                LiveProbe::Released(storage);
                freeCount_.fetch_add(1, std::memory_order_relaxed);
            }
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
            LiveProbe::Acquired(storage, byteCount, alignment);
            return storage;
        }

        static void ReleaseAligned(void *const storage) noexcept {
            if (storage != nullptr) {
                LiveProbe::Released(storage);
                freeCount_.fetch_add(1, std::memory_order_relaxed);
            }
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
            failureObserver_ = observer;
            failureCountdown_ = successfulAllocations;
        }

        static void DisableFailures() noexcept {
            failureCountdown_ = DisabledFailureCountdown;
            failureObserver_ = nullptr;
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
            if (failureCountdown_ == DisabledFailureCountdown)
                return;
            if (failureCountdown_ != 0) {
                --failureCountdown_;
                return;
            }
            const auto observer = failureObserver_;
            DisableFailures();
            if (observer != nullptr)
                observer(byteCount);
            throw std::bad_alloc{};
        }

        static constexpr std::size_t DisabledFailureCountdown = std::numeric_limits<std::size_t>::max();
        static inline std::atomic<std::size_t> count_{};
        static inline std::atomic<std::size_t> freeCount_{};
        // Only the scope's calling thread may consume its failure or invoke its observer.
        static inline thread_local std::size_t failureCountdown_{DisabledFailureCountdown};
        static inline thread_local Horo::Tests::AllocationProbe::FailureObserver failureObserver_{};
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

    ScopedLiveMeasurement::ScopedLiveMeasurement(const std::span<LiveAllocation> storage) noexcept : storage_(storage) {
        const std::lock_guard lock(liveProbeMutex);
        measurement_.incomplete = activeLiveProbe.load(std::memory_order_relaxed) != nullptr || storage_.empty();
        if (!measurement_.incomplete)
            activeLiveProbe.store(this, std::memory_order_release);
    }

    ScopedLiveMeasurement::~ScopedLiveMeasurement() {
        const std::lock_guard lock(liveProbeMutex);
        if (activeLiveProbe.load(std::memory_order_relaxed) == this)
            activeLiveProbe.store(nullptr, std::memory_order_release);
    }

    LiveMeasurement ScopedLiveMeasurement::Snapshot() const noexcept {
        const std::lock_guard lock(liveProbeMutex);
        return measurement_;
    }

    void ScopedLiveMeasurement::Acquired(void *const address, const std::size_t requestedBytes, const std::size_t alignment) noexcept {
        if (activeLiveProbe.load(std::memory_order_acquire) == nullptr)
            return;
        const std::lock_guard lock(liveProbeMutex);
        auto *probe = activeLiveProbe.load(std::memory_order_relaxed);
        if (probe == nullptr)
            return;
        auto &sample = probe->measurement_;
        const auto usableBytes = UsableAllocationBytes(address, alignment);
        constexpr auto maxSize = std::numeric_limits<std::size_t>::max();
        if (probe->count_ == probe->storage_.size() || usableBytes == 0 || usableBytes == maxSize || usableBytes < requestedBytes ||
            usableBytes > maxSize - sample.liveUsableBytes) {
            sample.incomplete = true;
            return;
        }
        probe->storage_[probe->count_++] = {address, requestedBytes, usableBytes};
        sample.liveBytes += requestedBytes;
        sample.peakBytes = std::max(sample.peakBytes, sample.liveBytes);
        sample.liveUsableBytes += usableBytes;
        sample.peakUsableBytes = std::max(sample.peakUsableBytes, sample.liveUsableBytes);
    }

    void ScopedLiveMeasurement::Released(const void *address) noexcept {
        if (activeLiveProbe.load(std::memory_order_acquire) == nullptr)
            return;
        const std::lock_guard lock(liveProbeMutex);
        auto *probe = activeLiveProbe.load(std::memory_order_relaxed);
        if (probe == nullptr)
            return;
        const auto live = probe->storage_.first(probe->count_);
        const auto found = std::ranges::find(live, address, &LiveAllocation::address);
        if (found == live.end())
            return;
        probe->measurement_.liveBytes -= found->requestedBytes;
        probe->measurement_.liveUsableBytes -= found->usableBytes;
        *found = probe->storage_[--probe->count_];
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
