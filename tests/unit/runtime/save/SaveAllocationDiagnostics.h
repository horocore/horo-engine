#pragma once

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSlotLifecycle.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Horo::Runtime::SaveSlotLifecycleTest {
    /** @brief Emits one bounded literal to native stderr without entering the C++ allocator or stream machinery. */
    template <std::size_t Size> void SaveAllocationPhase(const char (&message)[Size]) noexcept {
        static_assert(Size <= 160);
#ifdef _WIN32
        const DWORD previousError = ::GetLastError();
        const HANDLE output = ::GetStdHandle(STD_ERROR_HANDLE);
        if (output != nullptr && output != INVALID_HANDLE_VALUE) {
            DWORD written{};
            // No formatting, buffering, flushing, or retry loop in an armed failure window.
            (void)::WriteFile(output, message, static_cast<DWORD>(Size - 1), &written, nullptr);
        }
        ::SetLastError(previousError);
#else
        (void)message;
#endif
    }

    /** @brief Marks the exact one-shot injection immediately before bad_alloc is thrown. */
    inline void SaveAllocationInjected(const std::size_t) noexcept {
        SaveAllocationPhase("[save-allocation] injected-before-throw\n");
    }

    /** @brief Checks publication acknowledgement only after the injected failure scope has been reset. */
    inline void CheckSaveAllocationPublication(const Result<SaveSlotLifecycleResult> &result, const bool syncFailure) {
        REQUIRE(result.HasError() == syncFailure);
        if (syncFailure)
            CHECK(result.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
        else
            CHECK(result.Value().cleanupDeferred);
    }

    /** @brief Declared before the fixture so its final marker also covers fixture teardown during exception unwinding. */
    struct SaveAllocationFixtureExit final {
        ~SaveAllocationFixtureExit() {
            SaveAllocationPhase("[save-allocation] fixture-scope-exited\n");
        }
    };
}  // namespace Horo::Runtime::SaveSlotLifecycleTest
