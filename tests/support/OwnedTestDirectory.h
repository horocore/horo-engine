#pragma once

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Tests {
    /** @brief Owns only an exclusively created directory in the trusted test working directory, never a shared temp path. */
    class OwnedTestDirectory final {
    public:
        explicit OwnedTestDirectory(const std::string_view prefix)
            : path_(std::filesystem::current_path() /
                    (std::string{prefix} + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            // An existing directory or symlink must fail admission, not be adopted or deleted.
            REQUIRE(std::filesystem::create_directory(path_));
        }

        ~OwnedTestDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        OwnedTestDirectory(const OwnedTestDirectory &) = delete;
        OwnedTestDirectory &operator=(const OwnedTestDirectory &) = delete;
        OwnedTestDirectory(OwnedTestDirectory &&) = delete;
        OwnedTestDirectory &operator=(OwnedTestDirectory &&) = delete;

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };
}  // namespace Horo::Tests
