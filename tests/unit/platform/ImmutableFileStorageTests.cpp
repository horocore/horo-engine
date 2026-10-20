#include "Horo/Foundation/ErrorCodeRegistry.h"
#include "Horo/Platform/NativeImmutableFileStorage.h"
#include "OwnedTestDirectory.h"

#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
    using Horo::FileStorageErrors::Conflict;
    using Horo::Platform::CreateNativeImmutableFileStorage;
    using Horo::Tests::OwnedTestDirectory;
    const std::vector<std::uint8_t> Payload{0, 1, 127, 128, 255};

    /** @brief Seeds a complete fixture file without granting the storage implementation a native fallback. */
    void Seed(const std::filesystem::path &path, const std::string_view text) {
        std::ofstream file(path, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(file.good());
    }

#if !defined(_WIN32)
    /** @brief Restores the fixture's exact original mode before its directory owner runs cleanup, including failed assertions. */
    class PermissionRestore final {
    public:
        explicit PermissionRestore(std::filesystem::path path)
            : path_(std::move(path)), permissions_(std::filesystem::status(path_).permissions()) {}

        ~PermissionRestore() {
            std::error_code error;
            std::filesystem::permissions(path_, permissions_, error);
        }

        PermissionRestore(const PermissionRestore &) = delete;
        PermissionRestore &operator=(const PermissionRestore &) = delete;

    private:
        std::filesystem::path path_;
        std::filesystem::perms permissions_;
    };
#endif
}  // namespace

TEST_CASE("Immutable storage failures register as an explicit inert host contribution", "[platform][storage]") {
    Horo::ModuleDescriptor contribution{.id = Horo::ModuleId{"horo.foundation.file_storage"}, .version = {1, 0, 0}};
    contribution.errorDomains.push_back(Horo::ImmutableFileStorageErrorDomain());
    const std::array modules{contribution};
    const auto registry = Horo::BuildErrorCodeRegistry(modules);
    REQUIRE(registry.HasValue());
    for (const auto *descriptor : contribution.errorDomains.front().descriptors)
        CHECK(registry.Value().Resolve(descriptor->domain, descriptor->code) != nullptr);
}

TEST_CASE("Rooted immutable storage keeps construction and missing loads free of output mutations", "[platform][storage]") {
    OwnedTestDirectory root{"immutable storage"};
    auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage");
    REQUIRE(storage.HasValue());
    CHECK(CreateNativeImmutableFileStorage(root.Path(), "../outside").HasError());
    CHECK(CreateNativeImmutableFileStorage(root.Path(), "").HasError());
    const auto loaded = storage.Value()->Load("absent/nested/entry", 1024, {});
    REQUIRE(loaded.HasValue());
    CHECK_FALSE(loaded.Value());
    CHECK(std::filesystem::is_empty(root.Path()));
    CHECK(CreateNativeImmutableFileStorage(root.Path() / "missing", "storage").HasError());
    CHECK_FALSE(std::filesystem::exists(root.Path() / "missing"));
}

TEST_CASE("Rooted immutable storage round trips binary and empty bytes under portable Unicode names", "[platform][storage]") {
    OwnedTestDirectory root{"immutable bytes"};
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    const auto bytes = GENERATE(Payload, std::vector<std::uint8_t>{});
    constexpr std::string_view path{"nested output/entry \xc3\xbc.cooked"};
    REQUIRE(storage->Store(path, bytes, {}).HasValue());
    REQUIRE(storage->Store(path, bytes, {}).HasValue());
    const auto loaded = storage->Load(path, 1024, {});
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value());
    CHECK(*loaded.Value() == bytes);
    std::size_t files{};
    for (const auto &entry : std::filesystem::recursive_directory_iterator(root.Path())) {
        CHECK_FALSE(entry.path().filename().string().starts_with(".horo-immutable-"));
        files += entry.is_regular_file() ? 1U : 0U;
    }
    CHECK(files == 1);
}

TEST_CASE("Rooted immutable storage rejects different existing bytes and enforces load bounds", "[platform][storage]") {
    OwnedTestDirectory root{"immutable conflict"};
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    REQUIRE(storage->Store("entry", Payload, {}).HasValue());
    for (const auto &other : {std::vector<std::uint8_t>{42}, std::vector<std::uint8_t>{5, 4, 3, 2, 1}}) {
        const auto replaced = storage->Store("entry", other, {});
        REQUIRE(replaced.HasError());
        CHECK(replaced.ErrorValue().code.Value() == Conflict.code.Value());
        CHECK(storage->Load("entry", 1024, {}).Value() == Payload);
    }
    CHECK(storage->Load("entry", Payload.size() - 1, {}).HasError());
    CHECK(storage->Load("entry", 0, {}).HasError());
    CHECK(storage->Load("entry", Horo::ImmutableFileStorage::MaximumEntryBytes + 1, {}).HasError());
}

TEST_CASE("Rooted immutable storage rejects nonportable paths and cancellation before directory creation", "[platform][storage]") {
    OwnedTestDirectory root{"immutable admission"};
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    for (const auto path : {"", "/absolute", "../outside", "a/../b", "a//b", "a\\b", "C:/entry", "NUL.cooked", "trailing.", "a/"}) {
        CHECK(storage->Load(path, 1024, {}).HasError());
        CHECK(storage->Store(path, Payload, {}).HasError());
    }
    const std::string_view embeddedNul{"a/\0b", 4};
    CHECK(storage->Load(embeddedNul, 1024, {}).HasError());
    CHECK(storage->Store(embeddedNul, Payload, {}).HasError());
    Horo::CancellationSource cancelled;
    cancelled.RequestCancellation();
    CHECK(storage->Load("missing/entry", 1024, cancelled.Token()).HasError());
    CHECK(storage->Store("missing/entry", Payload, cancelled.Token()).HasError());
    CHECK(std::filesystem::is_empty(root.Path()));
}

#if !defined(_WIN32)
TEST_CASE("Rooted immutable storage preserves existing bytes when native directory write permission is denied", "[platform][storage]") {
    if (geteuid() == 0)
        SKIP("A privileged test user bypasses ordinary POSIX directory permissions");
    OwnedTestDirectory root{"immutable permission denial"};
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    REQUIRE(storage->Store("restricted/entry", Payload, {}).HasValue());
    const auto restricted = root.Path() / "storage" / "restricted";
    const PermissionRestore restore{restricted};
    using enum std::filesystem::perms;
    std::filesystem::permissions(restricted, owner_read | owner_exec);
    const auto rejected = storage->Store("restricted/new", Payload, {});
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == Horo::FileStorageErrors::IoFailed.code.Value());
    CHECK_FALSE(std::filesystem::exists(restricted / "new"));
    CHECK(storage->Load("restricted/entry", 1024, {}).Value() == Payload);
}
#endif

TEST_CASE("Rooted immutable storage retains its granted identity after the original root spelling is replaced", "[platform][storage]") {
    OwnedTestDirectory parent{"immutable retained root"};
    const auto original = parent.Path() / "root";
    const auto retained = parent.Path() / "retained";
    REQUIRE(std::filesystem::create_directory(original));
    const auto storage = CreateNativeImmutableFileStorage(original, "storage").Value();
    std::filesystem::rename(original, retained);
    REQUIRE(std::filesystem::create_directory(original));
    REQUIRE(std::filesystem::create_directory(original / "storage"));
    Seed(original / "storage" / "victim", "untouched");
    REQUIRE(storage->Store("nested/entry", Payload, {}).HasValue());
    CHECK(std::filesystem::exists(retained / "storage" / "nested" / "entry"));
    CHECK_FALSE(std::filesystem::exists(original / "storage" / "nested"));
    CHECK(storage->Load("nested/entry", 1024, {}).Value() == Payload);
    CHECK_FALSE(storage->Load("victim", 1024, {}).Value());
}

TEST_CASE("Rooted immutable storage refuses file and directory redirects without modifying their targets", "[platform][storage]") {
    OwnedTestDirectory parent{"immutable redirects"};
    const auto root = parent.Path() / "root";
    const auto outside = parent.Path() / "outside";
    REQUIRE(std::filesystem::create_directories(root / "storage"));
    REQUIRE(std::filesystem::create_directory(outside));
    Seed(outside / "victim", "untouched");
    const auto storage = CreateNativeImmutableFileStorage(root, "storage").Value();
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "storage" / "redirect", error);
    if (error)
        SKIP("Native test user cannot create a directory symlink");
    std::filesystem::create_symlink(outside / "victim", root / "storage" / "entry");
    CHECK(storage->Load("redirect/victim", 1024, {}).HasError());
    CHECK(storage->Store("redirect/new", Payload, {}).HasError());
    CHECK(storage->Load("entry", 1024, {}).HasError());
    CHECK(storage->Store("entry", Payload, {}).HasError());
    CHECK(CreateNativeImmutableFileStorage(root / "storage" / "redirect", "storage").HasError());
    CHECK_FALSE(std::filesystem::exists(outside / "new"));
    CHECK(std::filesystem::file_size(outside / "victim") == 9);
}

TEST_CASE("Rooted immutable storage rejects multiple links and directory obstructions", "[platform][storage]") {
    OwnedTestDirectory root{"immutable object admission"};
    REQUIRE(std::filesystem::create_directory(root.Path() / "storage"));
    Seed(root.Path() / "storage" / "original", "untouched");
    std::filesystem::create_hard_link(root.Path() / "storage" / "original", root.Path() / "storage" / "entry");
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    CHECK(storage->Load("entry", 1024, {}).HasError());
    CHECK(storage->Store("entry", Payload, {}).HasError());
    CHECK(storage->Load("original/child", 1024, {}).HasError());
    CHECK(storage->Store("original/child", Payload, {}).HasError());
    REQUIRE(std::filesystem::create_directory(root.Path() / "storage" / "directory"));
    CHECK(storage->Load("directory", 1024, {}).HasError());
    CHECK(storage->Store("directory", Payload, {}).HasError());
    CHECK(std::filesystem::file_size(root.Path() / "storage" / "original") == 9);
}

TEST_CASE("Rooted immutable storage arbitrates simultaneous writers without overwriting the winner", "[platform][storage]") {
    OwnedTestDirectory root{"immutable competing writers"};
    const auto storage = CreateNativeImmutableFileStorage(root.Path(), "storage").Value();
    const bool different = GENERATE(false, true);
    const auto second = different ? std::vector<std::uint8_t>{9, 8, 7} : Payload;
    std::array<std::optional<Horo::Result<void>>, 2> results;
    std::barrier start{3};
    std::jthread first{[&start, &results, storage] {
        start.arrive_and_wait();
        results[0] = storage->Store("shared/entry", Payload, {});
    }};
    std::jthread other{[&start, &results, storage, &second] {
        start.arrive_and_wait();
        results[1] = storage->Store("shared/entry", second, {});
    }};
    start.arrive_and_wait();
    first.join();
    other.join();
    REQUIRE(results[0]);
    REQUIRE(results[1]);
    const auto successes = static_cast<int>(results[0]->HasValue()) + static_cast<int>(results[1]->HasValue());
    CHECK(successes == (different ? 1 : 2));
    for (const auto &result : results)
        if (result->HasError())
            CHECK(result->ErrorValue().code.Value() == Conflict.code.Value());
    const auto loaded = storage->Load("shared/entry", 1024, {}).Value();
    REQUIRE(loaded);
    CHECK((*loaded == Payload || *loaded == second));
}
