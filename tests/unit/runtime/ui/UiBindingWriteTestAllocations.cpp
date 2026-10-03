#include "UiBindingWriteTestFixture.h"

#include <cstdlib>
#include <new>

namespace Horo::Runtime::Ui::BindingWriteTests {
    std::atomic<std::size_t> &WriteAllocations() noexcept {
        static std::atomic<std::size_t> allocations{};
        return allocations;
    }
}  // namespace Horo::Runtime::Ui::BindingWriteTests

void *operator new(const std::size_t bytes) {
    Horo::Runtime::Ui::BindingWriteTests::WriteAllocations().fetch_add(1);
    if (void *memory = std::malloc(bytes == 0 ? 1 : bytes))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}
