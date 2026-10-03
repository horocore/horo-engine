#include "MixerAllocationFailureOutcome.h"
#include "MixerTestFixture.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>

#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
#include <Windows.h>

// DbgHelp requires Windows SDK types; keep this dependent include in a separate block.
#include <DbgHelp.h>
#endif

using namespace Horo::Tests::MixerFixture;

namespace {
    bool failureInjected{};
    std::size_t failedBytes{};

#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    std::array<void *, 32> failureStack{};
    USHORT failureDepth{};

    /** @brief Require both the typed proxy allocator and the STL's noexcept-constructor allocation path. */
    bool SymbolizeFailureStack(const HANDLE process) noexcept {
        bool proxyAllocator{};
        bool proxyConstructor{};
        alignas(SYMBOL_INFO) std::array<std::byte, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
        auto *const symbol = reinterpret_cast<SYMBOL_INFO *>(storage.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        for (USHORT frame = 0; frame < failureDepth; ++frame) {
            DWORD64 displacement{};
            if (SymFromAddr(process, reinterpret_cast<DWORD64>(failureStack[frame]), &displacement, symbol)) {
                std::fprintf(stderr, "allocation frame: %s\n", symbol->Name);
                const std::string_view name{symbol->Name};
                proxyAllocator = proxyAllocator || name.find("std::allocator<std::_Container_proxy>::allocate") != std::string_view::npos;
                proxyConstructor = proxyConstructor || name.find("std::_Container_base12::_Alloc_proxy") != std::string_view::npos;
            } else {
                std::fprintf(stderr, "allocation frame unresolved: %p, Windows error: %lu\n", failureStack[frame], GetLastError());
            }
        }
        return proxyAllocator && proxyConstructor;
    }

    /** @brief Symbolize only the throwing allocation's recorded stack; never exempt a proxy from injection. */
    bool FailedDebugProxy() noexcept {
        if (!failureInjected || failedBytes != sizeof(std::_Container_proxy))
            return false;
        const HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
        if (!SymInitialize(process, nullptr, FALSE)) {
            std::fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
            return false;
        }
        const auto module = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));
        std::array<wchar_t, 32768> executable{};
        const DWORD pathLength = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        const bool loaded = pathLength != 0 && pathLength < executable.size() &&
                            SymLoadModuleExW(process, nullptr, executable.data(), nullptr, module, 0, nullptr, 0) != 0;
        if (!loaded)
            std::fprintf(stderr, "worker symbol module load failed: path length %lu, Windows error %lu\n", pathLength, GetLastError());
        const bool proxy = loaded && SymbolizeFailureStack(process);
        SymCleanup(process);
        return proxy;
    }
#endif

    /** @brief Record evidence without allocating inside global new. */
    void ObserveFailure(const std::size_t bytes) noexcept {
        failureInjected = true;
        failedBytes = bytes;
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        failureDepth = CaptureStackBackTrace(0, static_cast<DWORD>(failureStack.size()), failureStack.data(), nullptr);
#endif
    }

    /** @brief Exit promptly instead of waiting in a Debug CRT dialog; only proven STL proxy failures are expected. */
    [[noreturn]] void Terminated() noexcept {
        std::fprintf(stderr, "worker terminated: injected %d, request bytes %zu\n", static_cast<int>(failureInjected), failedBytes);
        AllocationFailureOutcome outcome = AllocationFailureOutcome::UnexpectedFailure;
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        std::fprintf(stderr, "MSVC iterator debug level %d, proxy bytes %zu, recorded frames %hu\n", _ITERATOR_DEBUG_LEVEL,
                     sizeof(std::_Container_proxy), failureDepth);
        if (FailedDebugProxy()) {
            std::fprintf(stderr, "MSVC checked-STL proxy allocation crossed a noexcept constructor\n");
            outcome = AllocationFailureOutcome::MsvcDebugProxyTerminated;
        }
#endif
        std::fflush(stderr);
        std::_Exit(static_cast<int>(outcome));
    }

    /** @brief Check the exact error and complete reclamation after the result leaves scope. */
    template <class Prepare> AllocationFailureOutcome CheckFailure(const std::size_t prefix, Prepare prepare) {
        // Warm lazy library state outside the measured failure attempt.
        {
            const auto warm = prepare();
            if (warm.HasError()) {
                std::fprintf(stderr, "worker warm preparation failed: %s\n", warm.ErrorValue().code.Value().c_str());
                return AllocationFailureOutcome::UnexpectedFailure;
            }
        }
        const std::size_t allocations = Horo::Tests::AllocationProbe::Count();
        const std::size_t frees = Horo::Tests::AllocationProbe::FreeCount();
        bool complete{};
        bool expectedError{};
        std::array<char, 128> errorCode{};
        {
            std::optional<decltype(prepare())> result;
            {
                Horo::Tests::AllocationProbe::ScopedFailure injected(prefix, ObserveFailure);
                result.emplace(prepare());
            }
            complete = result->HasValue();
            expectedError = result->HasError() && result->ErrorValue().code.Value() == AudioErrors::GraphBuildFailed.code.Value();
            if (result->HasError()) {
                const auto &code = result->ErrorValue().code.Value();
                std::copy_n(code.data(), std::min(code.size(), errorCode.size() - 1), errorCode.data());
            }
        }
        const std::size_t acquired = Horo::Tests::AllocationProbe::Count() - allocations;
        const std::size_t released = Horo::Tests::AllocationProbe::FreeCount() - frees;
        // Diagnostics run only after measurement and destruction, never in the throwing global-new observer.
        std::fprintf(stderr, "worker prefix %zu: injected %d, request bytes %zu, allocations %zu, frees %zu, complete %d, error %s\n",
                     prefix, static_cast<int>(failureInjected), failedBytes, acquired, released, static_cast<int>(complete),
                     errorCode.data());
        if (acquired != released + static_cast<std::size_t>(failureInjected))
            return AllocationFailureOutcome::UnexpectedFailure;
        if (complete && !failureInjected)
            return AllocationFailureOutcome::Complete;
        return expectedError && failureInjected ? AllocationFailureOutcome::Caught : AllocationFailureOutcome::UnexpectedFailure;
    }
}  // namespace

/** @brief Execute one failure prefix against the actual production target in a disposable process. */
int main(const int argc, const char *const *argv) {
    std::set_terminate(Terminated);
    if (argc != 3)
        return static_cast<int>(AllocationFailureOutcome::UnexpectedFailure);
    std::size_t prefix{};
    const std::string_view text{argv[2]};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), prefix);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return static_cast<int>(AllocationFailureOutcome::UnexpectedFailure);
    const MixerCompileProfile profile = Profile();
    AllocationFailureOutcome outcome{AllocationFailureOutcome::UnexpectedFailure};
    if (std::string_view{argv[1]} == "runtime") {
        const MixerRuntimeDescriptor descriptor{Scope, IdOf<AudioMemoryPoolId>(33), profile};
        outcome = CheckFailure(prefix, [&] {
            return MixerGraphRuntime::Create(descriptor);
        });
    } else if (std::string_view{argv[1]} == "compiler" || std::string_view{argv[1]} == "inserts") {
        const bool inserts = std::string_view{argv[1]} == "inserts";
        const MixerAssetSchema asset = Asset(inserts);
        Probe probe;
        Factory factory(probe);
        outcome = CheckFailure(prefix, [&] {
            return CompileMixerGraph(asset, Identity(), profile, inserts ? &factory : nullptr);
        });
    }
    return static_cast<int>(outcome);
}
