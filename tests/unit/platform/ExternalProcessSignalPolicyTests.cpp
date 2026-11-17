#include "Horo/Platform/ExternalProcess.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <pthread.h>
#include <signal.h>
#include <string>

namespace {
    /** @brief Owns temporary parent suppression while proving that native bootstrap changes only the child. */
    class ParentSignalPolicy final {
    public:
        ParentSignalPolicy() = default;
        ParentSignalPolicy(const ParentSignalPolicy &) = delete;
        ParentSignalPolicy &operator=(const ParentSignalPolicy &) = delete;

        ~ParentSignalPolicy() {
            for (std::size_t index = Signals.size(); index != 0; --index)
                if (installed_[index - 1])
                    static_cast<void>(sigaction(Signals[index - 1], &previous_[index - 1], nullptr));
            if (maskInstalled_)
                static_cast<void>(pthread_sigmask(SIG_SETMASK, &previousMask_, nullptr));
        }

        /** @brief Suppresses host signals with rollback for every partially installed policy. */
        bool Install() {
            struct sigaction ignored{};
            ignored.sa_handler = SIG_IGN;
            if (sigemptyset(&ignored.sa_mask) != 0)
                return false;
            for (std::size_t index = 0; index < Signals.size(); ++index) {
                if (sigaction(Signals[index], &ignored, &previous_[index]) != 0)
                    return false;
                installed_[index] = true;
            }
            sigset_t mask{};
            if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGTERM) != 0 || sigaddset(&mask, SIGUSR1) != 0 ||
                pthread_sigmask(SIG_BLOCK, &mask, &previousMask_) != 0)
                return false;
            maskInstalled_ = true;
            return true;
        }

        /** @brief Verifies that child setup leaves the parent's handlers and blocked signals intact. */
        bool IsInstalled() const {
            for (const int controlSignal : Signals) {
                struct sigaction disposition{};
                if (sigaction(controlSignal, nullptr, &disposition) != 0 || disposition.sa_handler != SIG_IGN)
                    return false;
            }
            sigset_t mask{};
            return pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 && sigismember(&mask, SIGTERM) == 1 &&
                   sigismember(&mask, SIGUSR1) == 1;
        }

    private:
        static constexpr std::array Signals{SIGINT, SIGTERM, SIGPIPE};
        std::array<struct sigaction, Signals.size()> previous_{};
        std::array<bool, Signals.size()> installed_{};
        sigset_t previousMask_{};
        bool maskInstalled_{};
    };
}  // namespace

TEST_CASE("External process control signals are independent of parent suppression", "[unit][platform][process]") {
    ParentSignalPolicy parent;
    REQUIRE(parent.Install());
    REQUIRE(parent.IsInstalled());
    Horo::NativeExternalProcessRunner runner;
    std::string output;
    const auto result = runner.Run({.executable = HORO_PROCESS_TEST_CHILD,
                                    .arguments = {"signal-policy"},
                                    .timeout = std::chrono::seconds{5},
                                    .onOutput =
                                        [&output](Horo::ProcessOutputLine line) {
        output += line.text + '\n';
    }},
                                   {});
    REQUIRE(result.HasValue());
    CHECK(result.Value().reason == Horo::ProcessTerminationReason::Exited);
    CHECK(result.Value().exitCode == 0);
    CHECK(output == "default-control-signals\n");
    const auto signalled =
        runner.Run({.executable = HORO_PROCESS_TEST_CHILD, .arguments = {"signalled"}, .timeout = std::chrono::seconds{5}}, {});
    REQUIRE(signalled.HasValue());
    CHECK(signalled.Value().reason == Horo::ProcessTerminationReason::Signalled);
    CHECK(signalled.Value().exitCode == 128 + SIGTERM);
    CHECK(signalled.Value().stopCause == Horo::ProcessStopCause::None);
    CHECK(parent.IsInstalled());
}
