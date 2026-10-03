#include "Horo/Assets/AssetPayloadCache.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <thread>

namespace {
    // This executable alone intercepts the exact payload allocation's deallocation.
    constinit std::atomic<const void *> TrackedPayload{};
    constinit std::atomic<bool> DeallocationEntered{};
    constinit std::atomic<bool> DeallocationAllowed{};

    /** @brief Always release the allocator barrier and join, including failed probe/assertion paths. */
    class WorkerRelease final {
    public:
        WorkerRelease(std::atomic<bool> &ready, std::jthread &worker) noexcept : ready_(ready), worker_(worker) {}

        ~WorkerRelease() {
            Finish();
        }

        void Finish() noexcept {
            DeallocationAllowed.store(true, std::memory_order_release);
            DeallocationAllowed.notify_one();
            ready_.store(true, std::memory_order_release);
            ready_.notify_one();
            if (worker_.joinable())
                worker_.join();
        }

    private:
        std::atomic<bool> &ready_;
        std::jthread &worker_;
    };

    /** @brief Observe physical ownership and attempt owner-thread admission while worker deallocation is paused. */
    [[nodiscard]] bool ProbePhysicalRelease() {
        auto created = Horo::Assets::AssetPayloadCache::Create(1, 4);
        if (created.HasError())
            return false;
        auto cache = std::move(created).Value();
        constexpr std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        auto retained = cache->Admit(payload);
        if (retained.HasError())
            return false;
        auto pin = std::move(retained).Value();
        const auto *allocation = pin.Bytes().data();
        cache->Evict(pin.Digest());
        std::atomic<bool> ready{};
        bool workerRead{};
        std::jthread worker{[lease = std::move(pin), &ready, &workerRead] {
            ready.wait(false, std::memory_order_acquire);
            workerRead = lease.Bytes().size() == 4 && lease.Bytes().front() == std::byte{1};
        }};
        WorkerRelease release{ready, worker};
        // Arm only after successful thread creation; failed construction cannot enter the barrier.
        TrackedPayload.store(allocation, std::memory_order_release);
        ready.store(true, std::memory_order_release);
        ready.notify_one();
        DeallocationEntered.wait(false, std::memory_order_acquire);
        const auto chargedBeforeFree = cache->Snapshot().retainedPayloadBytes;
        constexpr std::array replacement{std::byte{9}, std::byte{9}, std::byte{9}, std::byte{9}};
        const auto blocked = cache->Admit(replacement);
        release.Finish();
        const auto chargedAfterFree = cache->Snapshot().retainedPayloadBytes;
        const auto admittedAfterFree = cache->Admit(replacement);
        return chargedBeforeFree == 4 && blocked.HasError() && chargedAfterFree == 0 && admittedAfterFree.HasValue() && workerRead;
    }
}  // namespace

/** @brief Standard replacement allocator scoped to this isolated regression executable. */
void *operator new(const std::size_t size) {
    const auto memory = std::malloc(size == 0 ? 1 : size);
    if (!memory)
        throw std::bad_alloc{};
    return memory;
}

void *operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    if (memory && memory == TrackedPayload.load(std::memory_order_acquire)) {
        DeallocationEntered.store(true, std::memory_order_release);
        DeallocationEntered.notify_one();
        DeallocationAllowed.wait(false, std::memory_order_acquire);
        TrackedPayload.store(nullptr, std::memory_order_release);
    }
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    ::operator delete(memory);
}

int main() {
    try {
        if (ProbePhysicalRelease())
            return 0;
        std::fputs("Payload capacity was refunded before physical deallocation completed.\n", stderr);
        return 1;
    } catch (const std::exception &) {
        return 2;
    }
}
