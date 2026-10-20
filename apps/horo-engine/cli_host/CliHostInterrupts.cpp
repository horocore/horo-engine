#include "CliHostInterrupts.h"

#include "Horo/Cli/CliErrors.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Horo::Application::Internal {
    namespace {
        static_assert(std::atomic<unsigned>::is_always_lock_free);

        /** @brief Constant-initialized signal state; handlers use only bounded lock-free operations. */
        std::atomic<unsigned> &Pending() {
            static constinit std::atomic<unsigned> pending{};
            return pending;
        }

        /** @brief Serializes explicit process-native ownership; it grants no application capability. */
        std::atomic_flag &Active() {
            static constinit std::atomic_flag active{};
            return active;
        }

        /** @brief Records at most two pending pulses without invoking engine cancellation inside a native handler. */
        void RecordInterrupt() noexcept {
            if ((Pending().fetch_or(1U) & 1U) != 0)
                Pending().fetch_or(2U);
        }
#if defined(_WIN32)
        BOOL WINAPI Handler(DWORD event) {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
                return FALSE;
            RecordInterrupt();
            return TRUE;
        }
#else
        /** @brief No allocation, locking, callbacks or application access occurs in the native signal handler. */
        void Handler(int) {
            RecordInterrupt();
        }
#endif
    }  // namespace

    /** @copydoc NativeCliInterrupts::Start */
    Result<void> NativeCliInterrupts::Start(Cli::CliInvocationStopController &stop) {
        if (owns_ || Active().test_and_set())
            return Result<void>::Failure(MakeError(Cli::CliErrors::ExecutionContextInvalid));
        owns_ = true;
        Pending().store(0);
#if defined(_WIN32)
        if (!SetConsoleCtrlHandler(Handler, TRUE))
            return Result<void>::Failure(MakeError(Cli::CliErrors::HostFailure));
        interruptInstalled_ = true;
#else
        struct sigaction action{};
        action.sa_handler = Handler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, &interrupt_) != 0)
            return Result<void>::Failure(MakeError(Cli::CliErrors::HostFailure));
        interruptInstalled_ = true;
        if (sigaction(SIGTERM, &action, &terminate_) != 0)
            return Result<void>::Failure(MakeError(Cli::CliErrors::HostFailure));
        terminateInstalled_ = true;
#endif
        relay_ = std::jthread([&stop](std::stop_token shutdown) {
            while (!shutdown.stop_requested()) {
                const auto pending = Pending().exchange(0);
                if (pending != 0)
                    stop.Interrupt();
                if ((pending & 2U) != 0)
                    stop.Interrupt();
                // Bounded tooling-only relay; operation workers never wait on native signal handling.
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        });
        return Result<void>::Success();
    }

    /** @copydoc NativeCliInterrupts::~NativeCliInterrupts */
    NativeCliInterrupts::~NativeCliInterrupts() {
        if (!owns_)
            return;
        relay_.request_stop();
        if (relay_.joinable())
            relay_.join();
#if defined(_WIN32)
        if (interruptInstalled_)
            SetConsoleCtrlHandler(Handler, FALSE);
#else
        if (terminateInstalled_)
            sigaction(SIGTERM, &terminate_, nullptr);
        if (interruptInstalled_)
            sigaction(SIGINT, &interrupt_, nullptr);
#endif
        Active().clear();
    }
}  // namespace Horo::Application::Internal
