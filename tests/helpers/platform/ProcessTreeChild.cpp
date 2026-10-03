#include "Horo/Foundation/Platform.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
    void Heartbeat(const std::filesystem::path &path) {
        for (;;) {
            std::ofstream stream{path, std::ios::app};
            stream << 'x';
            stream.close();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    [[nodiscard]] bool SpawnDescendant(const char *executable, const char *marker) {
#if defined(_WIN32)
        std::string command = std::string{"\""} + executable + "\" descendant \"" + marker + "\"";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
#else
        static_cast<void>(executable);
        const pid_t child = fork();
        if (child == 0) {
            Heartbeat(marker);
            _exit(0);
        }
        return child > 0;
#endif
    }
}  // namespace

int main(const int argc, char **argv) {
    if (argc < 2)
        return 2;
    const std::string mode{argv[1]};
    if (mode == "exit-failure")
        return 17;
    if (mode == "adopt-probe-lease" && argc == 3) {
        Horo::NativeDurableFileSystem files;
        auto inherited = files.AdoptInheritedProductMaintenance(argv[2]);
        return inherited.HasValue() ? 0 : 4;
    }
    if (mode == "probe-lease-env-absent") {
#if defined(_WIN32)
        return GetEnvironmentVariableA("HORO_PRODUCT_PROBE_LEASE", nullptr, 0) == 0 ? 0 : 4;
#else
        return std::getenv("HORO_PRODUCT_PROBE_LEASE") == nullptr ? 0 : 4;
#endif
    }
#if defined(_WIN32)
    if (mode == "handle-unavailable" && argc == 3) {
        const auto value = static_cast<std::uintptr_t>(std::stoull(argv[2]));
        return WaitForSingleObject(reinterpret_cast<HANDLE>(value), 0) == WAIT_OBJECT_0 ? 4 : 0;
    }
#endif
#if !defined(_WIN32)
    if (mode == "signalled") {
        std::raise(SIGTERM);
        return 99;
    }
#endif
    if (mode == "flood") {
        std::cout << std::string(65536, 'o') << std::flush;
        std::cerr << std::string(65536, 'e') << std::flush;
        return 0;
    }
    if (mode == "descendant" && argc == 3) {
        Heartbeat(argv[2]);
        return 0;
    }
    if (mode == "tree" && argc == 3) {
        if (!SpawnDescendant(argv[0], argv[2]))
            return 3;
        std::cout << "ready\n" << std::flush;
    }
    if (mode == "exiting-tree" && argc == 3) {
        if (!SpawnDescendant(argv[0], argv[2]))
            return 3;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (!std::filesystem::exists(argv[2]) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        return std::filesystem::exists(argv[2]) ? 0 : 4;
    }
    if (mode == "stubborn") {
#if defined(_WIN32)
        SetConsoleCtrlHandler([](const DWORD) -> BOOL {
            return TRUE;
        }, TRUE);
#else
        std::signal(SIGTERM, SIG_IGN);
#endif
        std::cout << "ready\n" << std::flush;
    }
    std::this_thread::sleep_for(std::chrono::seconds{10});
    return 0;
}
