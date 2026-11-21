#include "Horo/Foundation/Platform.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <string>

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
                std::filesystem::current_path() / std::filesystem::path{u8"publication lock \u00fc \u56fe "}.concat(
                                                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            REQUIRE(std::filesystem::create_directory(root));
            return std::filesystem::canonical(root);
        }
    };

    TEST_CASE("Native directory publication is tracked and never replaces occupied destinations", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto prepared = directory.path / std::filesystem::path{u8"prepared ü 图"};
        const auto destination = directory.path / std::filesystem::path{u8"published ü 图"};
        REQUIRE(std::filesystem::create_directory(prepared));
        const std::string content{"complete payload"};
        REQUIRE(files.WriteDurable(prepared / "content", std::as_bytes(std::span{content.data(), content.size()})).HasValue());
        REQUIRE(files.SyncDirectory(prepared).HasValue());
        bool occupied{};
        SECTION("Absent destination commits") {}
        SECTION("Empty existing directory remains") {
            occupied = true;
            REQUIRE(std::filesystem::create_directory(destination));
        }
        SECTION("Existing nonempty directory remains") {
            occupied = true;
            REQUIRE(std::filesystem::create_directory(destination));
            std::ofstream(destination / "sentinel") << "foreign";
        }
        SECTION("Existing regular file remains") {
            occupied = true;
            std::ofstream(destination) << "foreign";
        }
        Horo::AtomicFileReplacementReceipt receipt;
        const auto published = files.PublishDirectoryNoReplaceTracked(prepared, destination, receipt);
        if (occupied) {
            CHECK(published.HasError());
            CHECK_FALSE(receipt.WasCommitted());
            CHECK(std::filesystem::is_regular_file(prepared / "content"));
            CHECK(std::filesystem::exists(destination));
        } else {
            REQUIRE(published.HasValue());
            CHECK(receipt.WasCommitted());
            CHECK_FALSE(std::filesystem::exists(prepared));
            CHECK(std::filesystem::is_regular_file(destination / "content"));
            REQUIRE(std::filesystem::create_directory(prepared));
            CHECK(files.PublishDirectoryNoReplaceTracked(prepared, directory.path / "other", receipt).HasError());
            CHECK(std::filesystem::is_directory(prepared));
            CHECK_FALSE(std::filesystem::exists(directory.path / "other"));
        }
    }

    TEST_CASE("Directory publication rejects malformed and nonsibling paths before mutation", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto prepared = directory.path / "prepared";
        REQUIRE(std::filesystem::create_directory(prepared));
        auto nul = (directory.path / "target").native();
        nul.push_back(std::filesystem::path::value_type{});
        nul.append(std::filesystem::path{"suffix"}.native());
        for (const auto &destination : {std::filesystem::path{"relative"}, prepared, directory.path / "nested" / "target",
                                        directory.path / "nested" / ".." / "target", std::filesystem::path{nul}}) {
            Horo::AtomicFileReplacementReceipt receipt;
            CHECK(files.PublishDirectoryNoReplaceTracked(prepared, destination, receipt).HasError());
            CHECK_FALSE(receipt.WasCommitted());
            CHECK(std::filesystem::is_directory(prepared));
        }
    }

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

    TEST_CASE("Native publication lock move construction transfers the last exclusion owner", "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto path = directory.path / "construction.lock";
        auto acquired = files.TryAcquireExclusive(path, "source owner");
        REQUIRE(acquired.HasValue());
        {
            Horo::ExclusiveFileLock transferred{std::move(acquired).Value()};
            CHECK(transferred.ProtectsPath(path));
            CHECK(files.TryAcquireExclusive(path, "while transferred").HasError());
        }
        // The original result is still alive: only a true ownership transfer permits reacquisition here.
        CHECK(files.TryAcquireExclusive(path, "after transferred destruction").HasValue());
    }

    TEST_CASE("Native publication lock move assignment releases the prior owner and transfers exclusion",
              "[unit][foundation][publication]") {
        LockDirectory directory;
        Horo::NativeDurableFileSystem files;
        const auto firstPath = directory.path / "first.lock";
        const auto secondPath = directory.path / "second.lock";
        auto first = files.TryAcquireExclusive(firstPath, "first owner");
        auto second = files.TryAcquireExclusive(secondPath, "second owner");
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        auto destination = std::move(first).Value();
        destination = std::move(second).Value();
        CHECK(files.TryAcquireExclusive(firstPath, "prior owner released").HasValue());
        CHECK(files.TryAcquireExclusive(secondPath, "transferred exclusion").HasError());
        destination = Horo::ExclusiveFileLock{};
        // Both original results remain alive, so retained source ownership would still block this acquisition.
        CHECK(files.TryAcquireExclusive(secondPath, "last owner released").HasValue());
    }
}  // namespace
