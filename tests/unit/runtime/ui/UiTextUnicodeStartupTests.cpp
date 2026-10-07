#include "Horo/Runtime/Ui/UiTextUnicode.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <unicode/uclean.h>
#include <utility>
#include <vector>

namespace {
    struct NativeAllocationState final {
        std::size_t requests{};
        std::size_t live{};
        std::size_t allowed{3};
    };
}  // namespace

extern "C" {
static void *HoroUnicodeTestAllocate(const void *context, const std::size_t bytes) noexcept {
    auto &state = *static_cast<NativeAllocationState *>(const_cast<void *>(context));
    if (state.requests++ >= state.allowed)
        return nullptr;
    auto *result = std::malloc(bytes);
    if (result != nullptr)
        ++state.live;
    return result;
}

static void *HoroUnicodeTestReallocate(const void *context, void *pointer, const std::size_t bytes) noexcept {
    auto &state = *static_cast<NativeAllocationState *>(const_cast<void *>(context));
    if (state.requests++ >= state.allowed)
        return nullptr;
    auto *result = std::realloc(pointer, bytes);
    if (pointer == nullptr && result != nullptr)
        ++state.live;
    return result;
}

static void HoroUnicodeTestFree(const void *context, void *pointer) noexcept {
    if (pointer != nullptr) {
        auto &state = *static_cast<NativeAllocationState *>(const_cast<void *>(context));
        --state.live;
        std::free(pointer);
    }
}
}

TEST_CASE("Unicode startup rolls partial native allocation back before releasing registered data", "[runtime_ui][unicode][startup]") {
    std::ifstream input(HORO_UNICODE_DATA_FILE, std::ios::binary);
    REQUIRE(input.good());
    std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(Horo::Runtime::Ui::UiTextUnicodeRuntime::Create({}).HasError());
    REQUIRE(Horo::Runtime::Ui::UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes)).first(bytes.size() - 1)).HasError());
    bytes.back() ^= 1;
    REQUIRE(Horo::Runtime::Ui::UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes))).HasError());
    bytes.back() ^= 1;
    NativeAllocationState state;
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(&state, HoroUnicodeTestAllocate, HoroUnicodeTestReallocate, HoroUnicodeTestFree, &status);
    REQUIRE(U_SUCCESS(status));
    auto result = Horo::Runtime::Ui::UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes)));
    if (result.HasValue()) {
        // Always release the allocator context before a failed assertion unwinds it.
        auto runtime = std::move(result).Value();
        REQUIRE(runtime.Shutdown().HasValue());
    }
    REQUIRE(result.HasError());
    REQUIRE(state.requests > state.allowed);
    REQUIRE(state.live == 0);
    REQUIRE(Horo::Runtime::Ui::UiTextUnicodeRuntime::Create(std::as_bytes(std::span(bytes))).HasError());
}
