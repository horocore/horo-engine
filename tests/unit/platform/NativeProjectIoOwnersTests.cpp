#include "../../support/AllocationProbe.h"
#include "../../support/OwnedTestDirectory.h"
#include "Horo/Platform/NativeProjectIoOwners.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Platform;

    /** @brief Owns both the admitted project and a potential replacement without relying on the root spelling for cleanup. */
    struct ProjectIoFixture {
        Tests::OwnedTestDirectory directory{"project-io"};
        std::filesystem::path anchor{std::filesystem::canonical(directory.Path())};
        std::filesystem::path root{anchor / "project"};

        ProjectIoFixture() {
            std::filesystem::create_directories(root / "authored");
            std::ofstream(root / "authored/source.txt") << "owned source";
        }

        ProjectReadContext Context() const {
            return {.projectIdentity = "project-io-test",
                    .projectGeneration = 7,
                    .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10}};
        }

        NativeProjectIoOwners Open() const {
            auto owners = CreateNativeProjectIoOwners(root, "project-io-test", 7, ".horo/cache", ".horo/cooked");
            REQUIRE(owners.HasValue());
            REQUIRE_FALSE(std::filesystem::exists(root / ".horo"));
            return std::move(owners).Value();
        }
    };

    constexpr std::array<std::uint8_t, 3> CacheBytes{1, 2, 3};
}  // namespace

TEST_CASE("Native project I/O admits authority identity before opening or creating derived output", "[platform][project-io]") {
    ProjectIoFixture fixture;
    auto root = fixture.root;
    std::string identity{"project-io-test"};
    std::uint64_t generation = 7;
    SECTION("empty identity") {
        identity.clear();
    }
    SECTION("oversized identity") {
        identity.assign(257, 'x');
    }
    SECTION("invalid UTF-8 identity") {
        identity = std::string(1, static_cast<char>(0xff));
    }
    SECTION("zero generation") {
        generation = 0;
    }
    SECTION("relative root") {
        root = "project";
    }
    SECTION("unnormalized root") {
        root /= "authored/..";
    }
    SECTION("missing root") {
        root /= "absent";
    }
    CHECK(CreateNativeProjectIoOwners(root, identity, generation, ".horo/cache", ".horo/cooked").HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Native project I/O rejects invalid and lexically overlapping output namespaces", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto invalid = GENERATE("", "../outside", "/absolute", "a/../b", "CON", "safe:stream");
    CHECK(CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, invalid, ".horo/cooked").HasError());
    CHECK(CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, ".horo/cache", invalid).HasError());
    CHECK(CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, "derived", "derived").HasError());
    CHECK(CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, "derived", "derived/child").HasError());
    CHECK(CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, "derived/child", "derived").HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo"));
    CHECK_FALSE(std::filesystem::exists(fixture.root / "derived"));
}

TEST_CASE("Native project I/O keeps all three owners on the original root after pathname replacement", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto owners = fixture.Open();
    const auto moved = fixture.anchor / "retained-project";
    std::filesystem::rename(fixture.root, moved);
    std::filesystem::create_directories(fixture.root / "authored");
    std::ofstream(fixture.root / "authored/source.txt") << "replacement source";
    const auto source = owners.source->SourceText("authored/source.txt", fixture.Context(), {});
    REQUIRE(source.HasValue());
    REQUIRE(source.Value().text);
    CHECK(*source.Value().text == "owned source");
    REQUIRE(owners.cache->Store("artifact", CacheBytes, {}).HasValue());
    REQUIRE(owners.publication->WriteDurable(fixture.root / ".horo/cooked/private", std::as_bytes(std::span{CacheBytes})).HasValue());
    CHECK(std::filesystem::exists(moved / ".horo/cache/artifact"));
    CHECK(std::filesystem::exists(moved / ".horo/cooked/private"));
    CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo"));
    const auto cached = owners.cache->Load("artifact", 3, {});
    REQUIRE(cached.HasValue());
    REQUIRE(cached.Value());
    CHECK(*cached.Value() == std::vector<std::uint8_t>{1, 2, 3});
    CHECK(owners.publication->ReadFileBounded(fixture.root / ".horo/cooked/private", 3).Value() == *cached.Value());
}

TEST_CASE("Native project I/O owners retain independent lifetimes and complete directory cursors", "[platform][project-io]") {
    ProjectIoFixture fixture;
    auto owners = fixture.Open();
    const auto source = std::move(owners.source);
    const auto cache = std::move(owners.cache);
    const auto publication = std::move(owners.publication);
    owners = {};
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto inventory = source->Files("authored", fixture.Context(), {});
        REQUIRE(inventory.HasValue());
        REQUIRE(inventory.Value().entries.size() == 1);
        CHECK(inventory.Value().entries.front().path == "authored/source.txt");
    }
    REQUIRE(cache->Store("artifact", CacheBytes, {}).HasValue());
    REQUIRE(publication->WriteDurable(fixture.root / ".horo/cooked/private", {}).HasValue());
    CHECK(source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
}

TEST_CASE("Native project I/O preserves original source identity generation stop and deadline fences", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto owners = fixture.Open();
    auto context = fixture.Context();
    SECTION("identity mismatch") {
        context.projectIdentity = "different-project";
    }
    SECTION("generation mismatch") {
        ++context.projectGeneration;
    }
    SECTION("revoked authority") {
        context.authorityStopped = [] {
            return true;
        };
    }
    SECTION("expired deadline") {
        context.deadline = std::chrono::steady_clock::now();
    }
    SECTION("cancelled") {
        CancellationSource stop;
        stop.RequestCancellation();
        context.cancellation = stop.Token();
    }
    CHECK(owners.source->SourceText("authored/source.txt", context, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo"));
}

TEST_CASE("Native project I/O retains spaces and Unicode project and output namespace spellings", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto root = fixture.anchor / std::filesystem::path{u8"project dir ü"};
    std::filesystem::rename(fixture.root, root);
    const auto owners = CreateNativeProjectIoOwners(root, "project-io-test", 7, "derived/cache ü", "derived/cook ü");
    REQUIRE(owners.HasValue());
    REQUIRE(owners.Value().source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
    REQUIRE(owners.Value().cache->Store("artifact", CacheBytes, {}).HasValue());
    REQUIRE(owners.Value().publication->WriteDurable(root / std::filesystem::path{u8"derived/cook ü/private"}, {}).HasValue());
}

TEST_CASE("Native project I/O reports an obstructed publication namespace as storage failure", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto owners = fixture.Open();
    std::filesystem::create_directory(fixture.root / ".horo");
    std::ofstream(fixture.root / ".horo/cooked") << "protected obstruction";
    const auto inspected = owners.publication->InspectEntry(fixture.root / ".horo/cooked");
    REQUIRE(inspected.HasError());
    CHECK(inspected.ErrorValue().code.Value() == FileStorageErrors::IoFailed.code.Value());
    CHECK(std::filesystem::is_regular_file(fixture.root / ".horo/cooked"));
    CHECK(owners.source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
}

TEST_CASE("Native project I/O allocation failures leave no partial output and permit complete retry", "[platform][project-io]") {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
    SKIP("MSVC checked-STL allocation injection requires process isolation across noexcept constructors");
#else
    ProjectIoFixture fixture;
    const auto warm = fixture.Open();
    NativeProjectIoOwners recovered;
    bool projectCapacity{};
    bool storageCapacity{};
    for (std::size_t prefix = 0; prefix < 256; ++prefix) {
        std::string identity{"project-io-test"};
        auto attempt = [&] {
            Tests::AllocationProbe::ScopedFailure failure{prefix};
            return CreateNativeProjectIoOwners(fixture.root, std::move(identity), 7, ".horo/cache", ".horo/cooked");
        }();
        CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo"));
        if (attempt.HasValue()) {
            recovered = std::move(attempt).Value();
            break;
        }
        const auto &code = attempt.ErrorValue().code.Value();
        projectCapacity = projectCapacity || code == ProjectReadErrors::Capacity.code.Value();
        storageCapacity = storageCapacity || code == FileStorageErrors::Capacity.code.Value();
        REQUIRE((code == ProjectReadErrors::Capacity.code.Value() || code == FileStorageErrors::Capacity.code.Value()));
    }
    REQUIRE(projectCapacity);
    REQUIRE(storageCapacity);
    REQUIRE(recovered.source);
    REQUIRE(recovered.cache);
    REQUIRE(recovered.publication);
    CHECK(recovered.source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
    CHECK(warm.source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
    REQUIRE(recovered.cache->Store("retry", CacheBytes, {}).HasValue());
    CHECK(recovered.publication->WriteDurable(fixture.root / ".horo/cooked/retry", std::as_bytes(std::span{CacheBytes})).HasValue());
#endif
}

#if !defined(_WIN32)
TEST_CASE("Native project I/O refuses redirected derived namespaces without mutating outside entries", "[platform][project-io]") {
    ProjectIoFixture fixture;
    const auto outside = fixture.anchor / "outside";
    std::filesystem::create_directory(outside);
    std::filesystem::create_directory(fixture.root / ".horo");
    std::filesystem::create_directory_symlink(outside, fixture.root / ".horo/cache");
    std::filesystem::create_directory_symlink(outside, fixture.root / ".horo/cooked");
    const auto owners = CreateNativeProjectIoOwners(fixture.root, "project-io-test", 7, ".horo/cache", ".horo/cooked");
    REQUIRE(owners.HasValue());
    CHECK(owners.Value().cache->Store("escape", CacheBytes, {}).HasError());
    CHECK(owners.Value().publication->WriteDurable(fixture.root / ".horo/cooked/escape", {}).HasError());
    CHECK(std::filesystem::is_empty(outside));
    CHECK(owners.Value().source->Text("authored/source.txt", fixture.Context(), {}).HasValue());
}
#endif
