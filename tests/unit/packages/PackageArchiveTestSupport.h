#pragma once

#include "Horo/Foundation/Sha256.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <miniz.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Tests::Packages {
    inline nlohmann::json FileInventoryEntry(const std::string_view path, const std::string_view content) {
        return {{"path", path},
                {"size", content.size()},
                {"sha256", FormatSha256(ComputeSha256(std::as_bytes(std::span{content})))},
                {"executable", false},
                {"contributionRoot", nullptr}};
    }

    class TemporaryDirectory final {
    public:
        explicit TemporaryDirectory(const std::string_view prefix = "horo-package-tests")
            : path_(std::filesystem::temp_directory_path() /
                    (std::string{prefix} + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' +
                     std::to_string(++Sequence()))) {
            std::filesystem::create_directories(path_);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator
                     iterator{path_, std::filesystem::directory_options::skip_permission_denied, error},
                 end;
                 !error && iterator != end; iterator.increment(error)) {
                std::filesystem::permissions(iterator->path(), std::filesystem::perms::owner_all, std::filesystem::perm_options::add,
                                             error);
                error.clear();
            }
            std::filesystem::remove_all(path_, error);
        }

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        [[nodiscard]] static std::atomic_uint64_t &Sequence() noexcept {
            static std::atomic_uint64_t sequence{0};
            return sequence;
        }

        std::filesystem::path path_;
    };

    inline std::vector<std::byte> FinalizeArchive(mz_zip_archive &zip);

    inline std::vector<std::byte> ValidPackageArchiveBytes() {
        const std::vector<std::pair<std::string, std::string>> files{{"horo-package.toml", "schemaVersion = 1\n"},
                                                                     {"assets/data.bin", "verified bytes"}};
        nlohmann::json entries = nlohmann::json::array();
        for (const auto &[name, content] : files)
            entries.push_back(FileInventoryEntry(name, content));
        const std::string inventory = nlohmann::json{{"schemaVersion", 1}, {"files", entries}}.dump();

        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
        for (const auto &[name, content] : files)
            REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&zip, "files.manifest.json", inventory.data(), inventory.size(), MZ_BEST_COMPRESSION));
        return FinalizeArchive(zip);
    }

    inline std::vector<std::byte> FinalizeArchive(mz_zip_archive &zip) {
        void *buffer = nullptr;
        std::size_t size = 0;
        REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
        const std::unique_ptr<void, decltype(&std::free)> owner{buffer, &std::free};
        const auto *bytes = static_cast<const std::byte *>(buffer);
        std::vector<std::byte> result(bytes, bytes + size);
        REQUIRE(mz_zip_writer_end(&zip));
        return result;
    }
}  // namespace Horo::Tests::Packages
