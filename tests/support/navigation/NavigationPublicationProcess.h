#pragma once

#include "Horo/Foundation/Platform.h"

#include <catch2/catch_test_macros.hpp>
#include <cerrno>

#if !defined(_WIN32)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Horo::Application::TestSupport {
    /** @brief Holds the writer lease in a distinct process created before any test scheduler threads. */
    class ChildWriterLease final {
    public:
        explicit ChildWriterLease(const std::filesystem::path &lockPath) {
            REQUIRE(pipe(ready_) == 0);
            REQUIRE(pipe(release_) == 0);
            child_ = fork();
            REQUIRE(child_ >= 0);
            if (child_ == 0) {
                close(ready_[0]);
                close(release_[1]);
                NativeDurableFileSystem files;
                auto lease = files.TryAcquireExclusive(lockPath, "navigation child publication owner");
                if (lease.HasError())
                    _exit(4);
                const char acquired = '1';
                if (write(ready_[1], &acquired, 1) != 1)
                    _exit(2);
                _exit(WaitForPipeEvent(release_[0], POLLHUP, 15000) ? 0 : 3);
            }
            close(ready_[1]);
            close(release_[0]);
            const bool held = WaitForPipeEvent(ready_[0], POLLIN, 5000);
            close(ready_[0]);
            if (!held)
                Release();
            REQUIRE(held);
        }

        ChildWriterLease(const ChildWriterLease &) = delete;
        ChildWriterLease &operator=(const ChildWriterLease &) = delete;

        ~ChildWriterLease() {
            Release();
        }

        void Release() noexcept {
            if (child_ <= 0)
                return;
            close(release_[1]);
            int status{};
            while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {
            }
            child_ = 0;
        }

    private:
        [[nodiscard]] static bool WaitForPipeEvent(const int descriptor, const short event, const int timeoutMilliseconds) noexcept {
            pollfd readiness{.fd = descriptor, .events = event, .revents = 0};
            return poll(&readiness, 1, timeoutMilliseconds) == 1 && (readiness.revents & event) != 0;
        }

        int ready_[2]{};
        int release_[2]{};
        pid_t child_{};
    };
}  // namespace Horo::Application::TestSupport
#endif
