#pragma once

/** @file CliHostInterrupts.h @brief Private invocation-owned native interrupt bridge for synchronous CLI commands. */
#include "Horo/Cli/CliDispatcher.h"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace Horo::Application::Internal {
    /** @brief Owns one process signal lease and a tooling-only stop relay; never composed by feature code.
     * The stop controller outlives this scope. The invocation thread starts/destroys the scope; the owned
     * relay alone consumes lock-free pending flags and joins before native handlers are restored.
     */
    class NativeCliInterrupts final {
    public:
        NativeCliInterrupts() = default;
        /** @brief Joins the relay and restores prior native handlers after invocation owners drain. */
        ~NativeCliInterrupts();
        NativeCliInterrupts(const NativeCliInterrupts &) = delete;
        NativeCliInterrupts &operator=(const NativeCliInterrupts &) = delete;
        NativeCliInterrupts(NativeCliInterrupts &&) = delete;
        NativeCliInterrupts &operator=(NativeCliInterrupts &&) = delete;
        /** @brief Installs explicit native ownership and relays first/repeated interrupts outside signal handlers.
         * @param stop Invocation-owned cooperative and force-token authority, retained until this scope drains.
         * @return Success or typed native/ownership failure; destruction restores any partially installed handlers.
         */
        [[nodiscard]] Result<void> Start(Cli::CliInvocationStopController &stop);

    private:
        bool owns_{};
        bool interruptInstalled_{};
#if !defined(_WIN32)
        bool terminateInstalled_{};
        struct sigaction interrupt_{};
        struct sigaction terminate_{};
#endif
        std::jthread relay_;
    };
}  // namespace Horo::Application::Internal
