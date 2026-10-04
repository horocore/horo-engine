#include "Horo/Foundation/Platform.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <fstream>

namespace {
    /** @brief Owns an exclusively created canonical root for native lock-file tests. */
    class LockDirectory final {
    public:
        const std::filesystem::path path{CreateRoot()};

        LockDirectory() = default;
        LockDirectory(const LockDirectory &) = delete;
        LockDirectory &operator=(const LockDirectory &) = delete;

        ~LockDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

    private:
        [[nodiscard]] static std::filesystem::path CreateRoot() {
            const auto root =
                std::filesystem::current_path() /
                std::filesystem::path{std::format("publication lock ü {}", std::chrono::steady_clock::now().time_since_epoch().count())};
            REQUIRE(std::filesystem::create_directory(root));
            return std::filesystem::canonical(root);
        }
    };

    TEST_CASE("Native publication locks reject malformed paths before filesystem changes", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto parent = directory.path / "uncreated";
        auto nulPath = (parent / "mutation.lock").native();
        nulPath.push_back(std::filesystem::path::value_type{});
        nulPath.append(std::filesystem::path{"suffix"}.native());
        for (const auto &path : {std::filesystem::path{"relative.lock"}, parent / ".." / "mutation.lock", parent / ".", parent / "",
                                 std::filesystem::path{nulPath}}) {
            CAPTURE(path);
            CHECK(files.TryAcquireExclusive(path, "invalid").HasError());
        }
        CHECK_FALSE(std::filesystem::exists(parent));
        CHECK(std::filesystem::is_empty(directory.path));
        const auto valid = directory.path / std::filesystem::path{u8"nav mesh ü.lock"};
        auto lock = files.TryAcquireExclusive(valid, "valid");
        REQUIRE(lock.HasValue());
        CHECK(lock.Value().ProtectsPath(valid));
    }

    TEST_CASE("Native publication locks share canonical authority through host directory aliases", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto nativeParent = directory.path / std::filesystem::path{u8"native root ü"};
        const auto aliasParent = directory.path / "host alias";
        REQUIRE(std::filesystem::create_directory(nativeParent));
        std::error_code error;
        std::filesystem::create_directory_symlink(nativeParent, aliasParent, error);
        if (error) {
            WARN("Native directory links unavailable on this test filesystem");
            return;
        }
        const auto suffix = std::filesystem::path{u8"new parent ü"} / "publication.lock";
        const auto canonicalPath = nativeParent / suffix;
        const auto aliasPath = aliasParent / suffix;
        {
            auto first = files.TryAcquireExclusive(aliasPath, "alias owner");
            REQUIRE(first.HasValue());
            CHECK(first.Value().ProtectsPath(canonicalPath));
            auto competing = files.TryAcquireExclusive(canonicalPath, "canonical contender");
            REQUIRE(competing.HasError());
            CHECK(competing.ErrorValue().code.Value() == "filesystem.lock_busy");
        }
        {
            auto first = files.TryAcquireExclusive(canonicalPath, "canonical owner");
            REQUIRE(first.HasValue());
            auto competing = files.TryAcquireExclusive(aliasPath, "alias contender");
            REQUIRE(competing.HasError());
            CHECK(competing.ErrorValue().code.Value() == "filesystem.lock_busy");
        }
        CHECK(files.TryAcquireExclusive(aliasPath, "after release").HasValue());
    }

    TEST_CASE("Native publication locks reject linked files without overwriting their contents", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto original = directory.path / "original";
        std::ofstream(original) << "keep";
        const auto alias = directory.path / "alias.lock";
        std::error_code error;
        SECTION("symlink") {
            std::filesystem::create_symlink(original, alias, error);
        }
        SECTION("hard link") {
            std::filesystem::create_hard_link(original, alias, error);
        }
        if (error) {
            WARN("Native file links unavailable on this test filesystem");
            return;
        }
        CHECK(files.TryAcquireExclusive(alias, "overwrite").HasError());
        std::ifstream input(original);
        std::string contents;
        input >> contents;
        CHECK(contents == "keep");
    }
}  // namespace
