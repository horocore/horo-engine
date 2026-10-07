#pragma once

#include <thread>
#include <utility>
#include <version>

/** @file
 * @brief Test-only join ownership compatible with standard libraries lacking C++20 jthread.
 * No stop token is part of these tests' contract: both implementations join before captured state is destroyed.
 * The override is confined to a diagnostic target; it never changes production configuration or feature support.
 */
namespace Horo::Tests::Jobs {
#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L && !defined(HORO_JOB_TEST_FORCE_THREAD_FALLBACK)
    using NativeThread = std::jthread;
#else
    // AppleClang/libc++ compositions without jthread use explicit scoped joining, not detached work or experimental flags.
    using NativeThread = std::thread;
#endif

    /** @brief Joins the native test thread on scope exit, including assertion unwinding. */
    class OwnedThread final {
    public:
        /** @brief Takes exclusive thread ownership. @param thread Joinable thread owned by this test scope. */
        explicit OwnedThread(NativeThread thread) : thread_(std::move(thread)) {}

        OwnedThread(const OwnedThread &) = delete;
        OwnedThread &operator=(const OwnedThread &) = delete;
        OwnedThread(OwnedThread &&) = delete;
        OwnedThread &operator=(OwnedThread &&) = delete;

        /** @brief Joins before dependent captures leave scope. */
        ~OwnedThread() {
            Join();
        }

        /** @brief Joins once; callers may synchronize explicitly before assertions. */
        void Join() {
            if (thread_.joinable())
                thread_.join();
        }

    private:
        NativeThread thread_;
    };
}  // namespace Horo::Tests::Jobs
