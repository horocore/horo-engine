#include <chrono>
#include <csignal>
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
        const pid_t child = fork();
        if (child == 0) {
            execl(executable, executable, "descendant", marker, static_cast<char *>(nullptr));
            _exit(127);
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
