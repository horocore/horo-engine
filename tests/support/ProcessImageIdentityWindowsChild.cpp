#include "ProcessImageIdentity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {
    /** @brief Only the parent's explicitly inherited event handles coordinate this owned child. */
    HANDLE Event(const wchar_t *argument) {
        wchar_t *end{};
        const auto value = std::wcstoull(argument, &end, 10);
        if (!value || !end || *end != L'\0' || value > UINTPTR_MAX)
            return nullptr;
        const auto handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
        DWORD flags{};
        return GetHandleInformation(handle, &flags) ? handle : nullptr;
    }

    /** @brief Bounds parent synchronization without changing any production identity input. */
    bool Rendezvous(HANDLE ready, HANDLE resume) {
        return SetEvent(ready) && WaitForSingleObject(resume, 10000) == WAIT_OBJECT_0;
    }

    int CaptureAfterReplacement(HANDLE ready, HANDLE resume) {
        if (!Rendezvous(ready, resume))
            return 10;
        DWORD before{}, after{};
        if (!GetProcessHandleCount(GetCurrentProcess(), &before))
            return 11;
        auto image = Horo::Platform::CaptureCurrentProcessImageIdentity();
        if (!GetProcessHandleCount(GetCurrentProcess(), &after))
            return 12;
        if (image.HasError()) {
            const bool kernelMismatch = image.ErrorValue().message.find("kernel did not bind") != std::string::npos;
            return kernelMismatch && before == after ? 7 : 13;
        }
        return image.Value().Verify().HasValue() && after >= before + 2 ? 0 : 14;
    }

    int RetainBeforeReplacement(HANDLE ready, HANDLE resume) {
        auto image = Horo::Platform::CaptureCurrentProcessImageIdentity();
        if (image.HasError())
            return 15;
        if (!Rendezvous(ready, resume))
            return 16;
        return image.Value().Verify().HasValue() ? 0 : 17;
    }
}  // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 4)
        return 18;
    const auto ready = Event(argv[2]);
    const auto resume = Event(argv[3]);
    if (!ready || !resume)
        return 19;
    if (std::wcscmp(argv[1], L"before") == 0)
        return CaptureAfterReplacement(ready, resume);
    if (std::wcscmp(argv[1], L"retained") == 0)
        return RetainBeforeReplacement(ready, resume);
    return 20;
}
