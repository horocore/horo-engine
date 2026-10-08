#pragma once
/** @file BenchmarkAllocationProbe.h
 * @brief Thread-local allocation probe shared by standalone single-translation-unit benchmarks.
 * @details Include exactly once per benchmark executable; never link into engine or regression test targets.
 */
#include <cstddef>
#include <cstdlib>
#include <new>

namespace Horo::Tests::BenchmarkAllocationProbe {
    /** @brief Calling-thread measurement state; no allocation is needed to start or reset a probe. */
    struct State final {
        bool trackAllocations{};
        std::size_t trackedAllocations{};
    };

    /** @brief Returns the calling thread's probe state. @return Thread-local allocation measurement. */
    inline State &AllocationState() noexcept {
        thread_local State state;
        return state;
    }
}  // namespace Horo::Tests::BenchmarkAllocationProbe

void *operator new(const std::size_t size) {
    if (auto &state = Horo::Tests::BenchmarkAllocationProbe::AllocationState(); state.trackAllocations)
        ++state.trackedAllocations;
    if (void *memory = std::malloc(size); memory != nullptr)
        return memory;
    throw std::bad_alloc{};
}

void *operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}
