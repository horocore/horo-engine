#pragma once
/** @file BenchmarkAllocationProbe.h
 * @brief Thread-local allocation probe shared by standalone single-translation-unit benchmarks.
 * @details Include exactly once per benchmark executable; never link into engine or regression test targets.
 */
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
    thread_local bool g_trackAllocations{};
    thread_local std::size_t g_trackedAllocations{};

}  // namespace

void *operator new(const std::size_t size) {
    if (g_trackAllocations)
        ++g_trackedAllocations;
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
