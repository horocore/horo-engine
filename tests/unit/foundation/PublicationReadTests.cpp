#include "../../support/OwnedTestDirectory.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
    using namespace Horo;

#if !defined(_WIN32)
    /** @brief Restores fixture permissions before its owning directory can be cleaned up, including assertion failures. */
    class RestrictedPermissions final {
    public:
        explicit RestrictedPermissions(std::filesystem::path path)
            : path_(std::move(path)), original_(std::filesystem::status(path_).permissions()) {
            std::filesystem::permissions(path_, std::filesystem::perms::none);
        }

        ~RestrictedPermissions() {
            std::error_code error;
            std::filesystem::permissions(path_, original_, error);
        }

        RestrictedPermissions(const RestrictedPermissions &) = delete;
        RestrictedPermissions &operator=(const RestrictedPermissions &) = delete;

    private:
        std::filesystem::path path_;
        std::filesystem::perms original_;
    };
#endif

    /** @brief A write-only authority counts mutation calls so read denial cannot hide a native fallback. */
    class WriteOnlyFiles final : public DurableFileSystem {
    public:
        std::size_t writes{};

        Result<bool> CreateDirectoryPath(const std::filesystem::path &, DirectoryCreationMode) override {
            return Refuse<bool>();
        }

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &, std::string_view) override {
            return Refuse<ExclusiveFileLock>();
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &) const override {
            return Refuse<std::uint64_t>();
        }

        Result<void> WriteDurable(const std::filesystem::path &, std::span<const std::byte>) override {
            ++writes;
            return Refuse<void>();
        }

        Result<void> CopyDurable(const std::filesystem::path &, const std::filesystem::path &) override {
            ++writes;
            return Refuse<void>();
        }

        Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) override {
            ++writes;
            return Refuse<void>();
        }

        Result<void> RemoveDurable(const std::filesystem::path &) override {
            ++writes;
            return Refuse<void>();
        }

        Result<void> SyncDirectory(const std::filesystem::path &) override {
            return Refuse<void>();
        }

    private:
        template <typename T> static Result<T> Refuse() {
            return Result<T>::Failure(MakeError(FileStorageErrors::IoFailed));
        }
    };
}  // namespace

TEST_CASE("Publication read primitives refuse absent capabilities without native fallback", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication authority"};
    NativeDurableFileSystem native;
    const auto path = directory.Path() / "existing";
    REQUIRE(native.WriteDurable(path, {}).HasValue());
    WriteOnlyFiles denied;
    CHECK(denied.ReadFileBounded(path, 1).HasError());
    CHECK(denied.InspectEntry(path).HasError());
    CHECK(denied.ListDirectory(directory.Path(), 1).HasError());
    CHECK(denied.writes == 0);
}

TEST_CASE("Native publication reads retain exact bounded bytes across multiple transfers", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication read ü"};
    NativeDurableFileSystem files;
    const auto path = directory.Path() / "encoded bytes ü";
    std::vector<std::uint8_t> bytes(8193, 0xD3);
    bytes[4096] = 0;
    REQUIRE(files.WriteDurable(path, std::as_bytes(std::span{bytes})).HasValue());
    const auto loaded = files.ReadFileBounded(path, bytes.size());
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value() == bytes);
    const auto metadata = files.InspectEntry(path);
    REQUIRE(metadata.HasValue());
    REQUIRE(metadata.Value());
    CHECK(metadata.Value()->kind == FileEntryKind::RegularFile);
    CHECK(metadata.Value()->sizeBytes == bytes.size());
    CHECK(metadata.Value()->hardLinks == 1);
    CHECK(files.ReadFileBounded(path, bytes.size() - 1).HasError());
    CHECK(files.ReadFileBounded(path, 0).HasError());
    CHECK(files.ReadFileBounded(path, std::numeric_limits<std::size_t>::max()).HasError());
    REQUIRE(files.WriteDurable(directory.Path() / "empty", {}).HasValue());
    const auto empty = files.ReadFileBounded(directory.Path() / "empty", 1);
    REQUIRE(empty.HasValue());
    CHECK(empty.Value().empty());
}

TEST_CASE("Native publication read admission and cancellation produce no output", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication read admission"};
    NativeDurableFileSystem files;
    const auto missing = directory.Path() / "missing";
    const auto absent = files.InspectEntry(missing);
    REQUIRE(absent.HasValue());
    CHECK_FALSE(absent.Value());
    CHECK(files.ReadFileBounded(missing, 1).HasError());
    CHECK(files.ReadFileBounded(directory.Path(), 1).HasError());
    CHECK(files.ReadFileBounded("relative", 1).HasError());
    CHECK(files.InspectEntry(directory.Path() / ".." / "escape").HasError());
    const auto ambiguous = directory.Path() / std::filesystem::path{std::string{"bad\0suffix", 10}};
    const auto invalid = files.InspectEntry(ambiguous);
    REQUIRE(invalid.HasError());
    CHECK(invalid.ErrorValue().code.Value() == FileStorageErrors::InvalidPath.code.Value());
    CHECK(files.ListDirectory(missing, 1).HasError());
    CancellationSource stop;
    stop.RequestCancellation();
    const auto cancelled = files.ReadFileBounded(missing, 1, stop.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == FileStorageErrors::Cancelled.code.Value());
    CHECK(files.ListDirectory(directory.Path(), 1, stop.Token()).HasError());
    CHECK_FALSE(std::filesystem::exists(missing));
}

TEST_CASE("Native inventory is immediate complete and fails instead of truncating at its bound", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication inventory ü"};
    NativeDurableFileSystem files;
    const auto empty = files.ListDirectory(directory.Path(), 1);
    REQUIRE(empty.HasValue());
    CHECK(empty.Value().empty());
    REQUIRE(files.CreateDirectoryPath(directory.Path() / "nested", DirectoryCreationMode::Single).HasValue());
    REQUIRE(files.WriteDurable(directory.Path() / "nested" / "child", {}).HasValue());
    REQUIRE(files.WriteDurable(directory.Path() / "manifest.json", {}).HasValue());
    const auto listed = files.ListDirectory(directory.Path(), 2);
    REQUIRE(listed.HasValue());
    REQUIRE(listed.Value().size() == 2);
    const auto folder = std::ranges::find(listed.Value(), std::filesystem::path{"nested"}, &FileDirectoryEntry::name);
    REQUIRE(folder != listed.Value().end());
    CHECK(folder->info.kind == FileEntryKind::Directory);
    CHECK(std::ranges::all_of(listed.Value(), [](const auto &entry) {
        return entry.name == entry.name.filename();
    }));
    const auto over = files.ListDirectory(directory.Path(), 1);
    REQUIRE(over.HasError());
    CHECK(over.ErrorValue().code.Value() == FileStorageErrors::Capacity.code.Value());
    CHECK(files.ListDirectory(directory.Path(), 0).HasError());
    CHECK(files.ListDirectory(directory.Path(), 1048577).HasError());
}

TEST_CASE("Native publication read rejects multiply linked files", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication hardlinks"};
    NativeDurableFileSystem files;
    const auto source = directory.Path() / "source";
    REQUIRE(files.WriteDurable(source, {}).HasValue());
    std::error_code error;
    std::filesystem::create_hard_link(source, directory.Path() / "alias", error);
    REQUIRE_FALSE(error);
    const auto info = files.InspectEntry(source);
    REQUIRE(info.HasValue());
    REQUIRE(info.Value());
    CHECK(info.Value()->hardLinks == 2);
    const auto loaded = files.ReadFileBounded(source, 1);
    REQUIRE(loaded.HasError());
    CHECK(loaded.ErrorValue().code.Value() == FileStorageErrors::UnsafeObject.code.Value());
}

#if !defined(_WIN32)
TEST_CASE("Native inventory classifies redirects without traversing and refuses FIFO reads without blocking",
          "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication special objects"};
    NativeDurableFileSystem files;
    REQUIRE(files.CreateDirectoryPath(directory.Path() / "folder", DirectoryCreationMode::Single).HasValue());
    std::filesystem::create_directory_symlink(directory.Path() / "folder", directory.Path() / "redirect");
    const auto info = files.InspectEntry(directory.Path() / "redirect");
    REQUIRE(info.HasValue());
    REQUIRE(info.Value());
    CHECK(info.Value()->kind == FileEntryKind::Redirect);
    CHECK(files.ListDirectory(directory.Path() / "redirect", 4).HasError());
    CHECK(files.InspectEntry(directory.Path() / "redirect" / "").HasError());
    CHECK(files.ListDirectory(directory.Path() / "redirect" / "", 4).HasError());
    REQUIRE(files.WriteDurable(directory.Path() / "folder" / "data", {}).HasValue());
    CHECK(files.ReadFileBounded(directory.Path() / "redirect" / "data", 1).HasError());
    CHECK(files.InspectEntry(directory.Path() / "redirect" / "data").HasError());
    REQUIRE(mkfifo((directory.Path() / "fifo").c_str(), 0600) == 0);
    CHECK(files.ReadFileBounded(directory.Path() / "fifo", 1).HasError());
    const auto entries = files.ListDirectory(directory.Path(), 3);
    REQUIRE(entries.HasValue());
    REQUIRE(entries.Value().size() == 3);
}

TEST_CASE("Native publication reads preserve permission failures and restore owned fixtures", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication denied access"};
    NativeDurableFileSystem files;
    const auto data = directory.Path() / "data";
    REQUIRE(files.WriteDurable(data, {}).HasValue());
    {
        const RestrictedPermissions restricted{data};
        const auto denied = files.ReadFileBounded(data, 1);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == FileStorageErrors::IoFailed.code.Value());
    }
    CHECK(files.ReadFileBounded(data, 1).HasValue());
    {
        const RestrictedPermissions restricted{directory.Path()};
        const auto denied = files.ListDirectory(directory.Path(), 4);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == FileStorageErrors::IoFailed.code.Value());
    }
    CHECK(files.ListDirectory(directory.Path(), 4).HasValue());
}
#endif

TEST_CASE("Native metadata inspection preserves unreadable file metadata without requiring identity evidence",
          "[platform][publication-read]") {
#if defined(_WIN32)
    SKIP("File mode bits do not model Windows content-read ACL denial.");
#else
    Tests::OwnedTestDirectory directory{"publication metadata permissions"};
    NativeDurableFileSystem files;
    const auto path = directory.Path() / "metadata";
    const std::string text{"metadata"};
    REQUIRE(files.WriteDurable(path, std::as_bytes(std::span{text.data(), text.size()})).HasValue());
    RestrictedPermissions restricted{path};
    if (files.ReadFileBounded(path, 32).HasValue())
        SKIP("This host's effective authority bypasses file mode read denial; metadata fallback cannot be exercised.");
    const auto inspected = files.InspectEntry(path);
    REQUIRE(inspected.HasValue());
    REQUIRE(inspected.Value());
    CHECK(inspected.Value()->kind == FileEntryKind::RegularFile);
    CHECK(inspected.Value()->sizeBytes == text.size());
    CHECK(inspected.Value()->hardLinks == 1);
    CHECK_FALSE(inspected.Value()->identity);
#endif
}

TEST_CASE("Native file identity is stable across reads and changes on same byte replacement", "[platform][publication-read]") {
    Tests::OwnedTestDirectory directory{"publication identity"};
    NativeDurableFileSystem files;
    const auto path = directory.Path() / "original";
    const auto replacement = directory.Path() / "replacement";
    const std::string text{"same"};
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    REQUIRE(files.WriteDurable(path, bytes).HasValue());
    const auto first = files.InspectEntry(path);
    REQUIRE(first.HasValue());
    REQUIRE(first.Value());
    REQUIRE(first.Value()->identity);
    REQUIRE(files.ReadFileBounded(path, 32).HasValue());
    CHECK(files.InspectEntry(path).Value()->identity == first.Value()->identity);
    REQUIRE(files.WriteDurable(replacement, bytes).HasValue());
    REQUIRE(files.AtomicReplace(replacement, path).HasValue());
    CHECK(files.InspectEntry(path).Value()->identity != first.Value()->identity);
}
