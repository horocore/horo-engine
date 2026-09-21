#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Horo::TestSupport {
    /** @brief Temporarily redirects the process home directory to an isolated test directory. */
    class ScopedTestHome final {
    public:
        /**
         * @brief Creates and activates an isolated home directory.
         * @param name Stable test-specific directory prefix.
         */
        explicit ScopedTestHome(const std::string_view name)
            : path_(std::filesystem::temp_directory_path() /
                    (std::string{name} + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
#if defined(_WIN32)
            constexpr const char *key = "USERPROFILE";
#else
            constexpr const char *key = "HOME";
#endif
            if (const char *current = std::getenv(key))
                previous_ = current;
            std::filesystem::create_directories(path_);
#if defined(_WIN32)
            _putenv_s(key, path_.string().c_str());
#else
            setenv(key, path_.string().c_str(), 1);
#endif
        }

        /** @brief Restores the original home directory and removes the isolated directory. */
        ~ScopedTestHome() {
#if defined(_WIN32)
            constexpr const char *key = "USERPROFILE";
            _putenv_s(key, previous_.value_or("").c_str());
#else
            constexpr const char *key = "HOME";
            if (previous_)
                setenv(key, previous_->c_str(), 1);
            else
                unsetenv(key);
#endif
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        ScopedTestHome(const ScopedTestHome &) = delete;
        ScopedTestHome &operator=(const ScopedTestHome &) = delete;

    private:
        std::filesystem::path path_;
        std::optional<std::string> previous_;
    };
}  // namespace Horo::TestSupport
