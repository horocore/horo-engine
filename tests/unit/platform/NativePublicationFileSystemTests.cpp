#include "../../support/OwnedTestDirectory.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Platform/NativePublicationFileSystem.h"

#include <catch2/generators/catch_generators.hpp>
#include <fstream>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
    using namespace Horo;
    const std::string Payload{"publication bytes ü"};

    /** @brief Owns a root outside the publication namespace and composes a real native backend without creating output. */
    struct Fixture {
        Tests::OwnedTestDirectory directory{"publication-root"};
        std::filesystem::path anchor{std::filesystem::canonical(directory.Path())};
        std::filesystem::path root{anchor / "derived" / "cook"};
        std::shared_ptr<DurableFileSystem> files;

        Fixture() {
            auto created = Platform::CreateNativePublicationFileSystem(anchor, "derived/cook");
            REQUIRE(created.HasValue());
            files = std::move(created).Value();
            REQUIRE_FALSE(std::filesystem::exists(anchor / "derived"));
        }

        void Write(const std::filesystem::path &path, const std::string_view text = Payload) {
            const auto result = files->WriteDurable(path, std::as_bytes(std::span{text}));
            INFO((result.HasError() ? result.ErrorValue().code.Value() : std::string_view{}));
            REQUIRE(result.HasValue());
        }
    };
}  // namespace

TEST_CASE("Rooted publication composes without creating output and rejects invalid namespaces", "[platform][publication]") {
    Fixture fixture;
    const auto name = GENERATE("", "../escape", "/absolute", "safe/../escape", "CON.txt", "safe\\escape", "safe:stream");
    CHECK(Platform::CreateNativePublicationFileSystem(fixture.anchor, name).HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.root));
    CHECK(fixture.files->InspectEntry(fixture.root).Value() == std::nullopt);
    CHECK(fixture.files->AvailableBytes(fixture.root).HasValue());
}

TEST_CASE("Rooted publication reads writes inspects enumerates copies and removes only its fixed namespace", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "source");
    REQUIRE(fixture.files->CopyDurable(fixture.root / "source", fixture.root / "copied").HasValue());
    const auto bytes = fixture.files->ReadFileBounded(fixture.root / "copied", 1024);
    REQUIRE(bytes.HasValue());
    CHECK(std::string{bytes.Value().begin(), bytes.Value().end()} == Payload);
    const auto info = fixture.files->InspectEntry(fixture.root / "source");
    REQUIRE(info.HasValue());
    REQUIRE(info.Value());
    CHECK(info.Value()->kind == FileEntryKind::RegularFile);
    CHECK(info.Value()->hardLinks == 1);
    CHECK(info.Value()->sizeBytes == Payload.size());
    REQUIRE(fixture.files->ListDirectory(fixture.root, 2).Value().size() == 2);
    CHECK(fixture.files->ListDirectory(fixture.root, 1).HasError());
    CHECK(fixture.files->ReadFileBounded(fixture.root / "source", 1).HasError());
    CHECK(fixture.files->RemoveDurable(fixture.root / "copied").HasValue());
    CHECK(fixture.files->RemoveDurable(fixture.root / "copied").HasValue());
    CHECK(fixture.files->SyncDirectory(fixture.root).HasValue());
    CHECK_FALSE(fixture.files->CreateDirectoryPath(fixture.root, DirectoryCreationMode::Single).Value());
}

TEST_CASE("Rooted publication rejects paths and leaves unauthorized parents untouched", "[platform][publication]") {
    Fixture fixture;
    const auto suffix = GENERATE("../authored", "../../authored", "new/bad:stream", "new/sub/../escape");
    const auto path = fixture.root / suffix;
    CHECK(fixture.files->WriteDurable(path, {}).HasError());
    CHECK(fixture.files->InspectEntry(path).HasError());
    CHECK(fixture.files->CreateDirectoryPath(path, DirectoryCreationMode::Parents).HasError());
    CHECK(fixture.files->TryAcquireExclusive(path, "test").HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.anchor / "derived"));
}

TEST_CASE("Rooted native publication preserves spaces and non-ASCII root namespace and leaf names", "[platform][publication]") {
    Fixture fixture;
    const auto anchor = fixture.anchor / std::filesystem::path{u8"project dir ü"};
    REQUIRE(std::filesystem::create_directory(anchor));
    const auto root = anchor / std::filesystem::path{u8"derived/cook ü"};
    const auto leaf = std::filesystem::path{u8"模型 çığ.bin"};
    auto files = Platform::CreateNativePublicationFileSystem(anchor, "derived/cook ü");
    REQUIRE(files.HasValue());
    REQUIRE(files.Value()->WriteDurable(root / leaf, std::as_bytes(std::span{Payload})).HasValue());
    const auto bytes = files.Value()->ReadFileBounded(root / leaf, 1024);
    REQUIRE(bytes.HasValue());
    CHECK(std::string(bytes.Value().begin(), bytes.Value().end()) == Payload);
    const auto inventory = files.Value()->ListDirectory(root, 1);
    REQUIRE(inventory.HasValue());
    REQUIRE(inventory.Value().size() == 1);
    CHECK(inventory.Value().front().name == leaf);
}

TEST_CASE("Rooted private creation never overwrites an existing entry and records native creation", "[platform][publication]") {
    Fixture fixture;
    REQUIRE(fixture.files->CreateDirectoryPath(fixture.root, DirectoryCreationMode::Parents).HasValue());
    bool created{};
    REQUIRE(fixture.files->WritePrivateDurable(fixture.root / "empty", {}, created).HasValue());
    CHECK(created);
    created = false;
    CHECK(fixture.files->WritePrivateDurable(fixture.root / "empty", {}, created).HasError());
    CHECK_FALSE(created);
    CHECK(fixture.files->ReadFileBounded(fixture.root / "empty", 1).Value().empty());
}

TEST_CASE("Rooted publication namespace remains a directory even when its parent already exists", "[platform][publication]") {
    Fixture fixture;
    std::filesystem::create_directory(fixture.anchor / "derived");
    bool created{};
    CHECK(fixture.files->WritePrivateDurable(fixture.root, {}, created).HasError());
    CHECK_FALSE(created);
    CHECK(fixture.files->TryAcquireExclusive(fixture.root, "invalid root lease").HasError());
    CHECK_FALSE(std::filesystem::exists(fixture.root));
}

TEST_CASE("Rooted selector replacement records native commit and directory promotion preserves contents", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "current", "old");
    fixture.Write(fixture.root / "prepared", "new");
    AtomicFileReplacementReceipt receipt;
    REQUIRE(fixture.files->AtomicReplaceTracked(fixture.root / "prepared", fixture.root / "current", receipt).HasValue());
    CHECK(receipt.WasCommitted());
    CHECK(fixture.files->ReadFileBounded(fixture.root / "current", 3).Value() == std::vector<std::uint8_t>{'n', 'e', 'w'});
    CHECK(fixture.files->AtomicReplaceTracked(fixture.root / "current", fixture.root / "other", receipt).HasError());
    fixture.Write(fixture.root / "staging" / "payload");
    REQUIRE(fixture.files->AtomicReplace(fixture.root / "staging", fixture.root / "generation").HasValue());
    CHECK(fixture.files->ReadFileBounded(fixture.root / "generation" / "payload", 1024).HasValue());
    CHECK(fixture.files->RemoveDurable(fixture.root / "generation").HasError());
    REQUIRE(fixture.files->RemoveDurable(fixture.root / "generation" / "payload").HasValue());
    CHECK(fixture.files->RemoveDurable(fixture.root / "generation").HasValue());
    CHECK(fixture.files->RemoveDurable(fixture.root).HasError());
}

TEST_CASE("Rooted replacement failure never fabricates native commit", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "prepared");
    AtomicFileReplacementReceipt receipt;
    CHECK(fixture.files->AtomicReplaceTracked(fixture.root / "prepared", fixture.anchor / "outside", receipt).HasError());
    CHECK_FALSE(receipt.WasCommitted());
    CHECK(fixture.files->ReadFileBounded(fixture.root / "prepared", 1024).HasValue());
    CHECK_FALSE(std::filesystem::exists(fixture.anchor / "outside"));
}

TEST_CASE("Rooted writer exclusion interoperates with classic native leases and survives authority destruction",
          "[platform][publication]") {
    Fixture fixture;
    NativeDurableFileSystem classic;
    const auto path = fixture.root / ".cook-writer.lock";
    auto first = fixture.files->TryAcquireExclusive(path, "first");
    REQUIRE(first.HasValue());
    CHECK(first.Value().ProtectsPath(path));
    CHECK_FALSE(first.Value().ProtectsPath(fixture.root / "other.lock"));
    const auto second = fixture.files->TryAcquireExclusive(path, "second");
    REQUIRE(second.HasError());
    CHECK(second.ErrorValue().code.Value() == "filesystem.lock_busy");
    CHECK(classic.TryAcquireExclusive(path, "classic").HasError());
    auto lease = std::move(first).Value();
    fixture.files.reset();
    CHECK(lease.ProtectsPath(path));
    lease = {};
    auto acquired = classic.TryAcquireExclusive(path, "classic");
    REQUIRE(acquired.HasValue());
    auto rooted = Platform::CreateNativePublicationFileSystem(fixture.anchor, "derived/cook");
    REQUIRE(rooted.HasValue());
    CHECK(rooted.Value()->TryAcquireExclusive(path, "rooted").HasError());
}

TEST_CASE("Rooted readers enforce cancellation and complete inventory bounds", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "data");
    CancellationSource stop;
    stop.RequestCancellation();
    const auto token = stop.Token();
    CHECK(fixture.files->ReadFileBounded(fixture.root / "data", 1024, token).HasError());
    CHECK(fixture.files->ListDirectory(fixture.root, 1, token).HasError());
    CHECK(fixture.files->ReadFileBounded(fixture.root / "data", 0).HasError());
    CHECK(fixture.files->ListDirectory(fixture.root, 0).HasError());
}

TEST_CASE("Rooted lease observation rejects a different file rebound at the same logical lock path", "[platform][publication]") {
    Fixture fixture;
    const auto path = fixture.root / "writer.lock";
    auto lease = fixture.files->TryAcquireExclusive(path, "owned");
    REQUIRE(lease.HasValue());
    std::filesystem::rename(path, fixture.root / "retained.lock");
    fixture.Write(path, "replacement");
    CHECK_FALSE(lease.Value().ProtectsPath(path));
    CHECK(fixture.files->ReadFileBounded(path, 1024).HasValue());
}

TEST_CASE("Rooted publication retains its original root when the old spelling is replaced", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "current", "owned");
    const auto moved = fixture.anchor / "moved";
    // Retain a root below the fixture so the fixture owner still removes both namespaces at teardown.
    const auto grantRoot = fixture.root;
    auto granted = Platform::CreateNativePublicationFileSystem(grantRoot, "child");
    REQUIRE(granted.HasValue());
    std::filesystem::rename(grantRoot, moved);
    std::filesystem::create_directories(grantRoot / "child");
    std::ofstream(grantRoot / "child" / "sentinel") << "unowned";
    REQUIRE(granted.Value()->WriteDurable(grantRoot / "child" / "new", {}).HasValue());
    CHECK(std::filesystem::exists(moved / "child" / "new"));
    CHECK_FALSE(std::filesystem::exists(grantRoot / "child" / "new"));
    CHECK(granted.Value()->ListDirectory(grantRoot / "child", 1).Value().size() == 1);
    CHECK(granted.Value()->ReadFileBounded(grantRoot / "child" / "sentinel", 1024).HasError());
}

#if !defined(_WIN32)
TEST_CASE("Rooted publication refuses redirects hard links and special files without outside mutation", "[platform][publication]") {
    Fixture fixture;
    fixture.Write(fixture.root / "owned");
    const auto outside = fixture.anchor / "outside";
    std::filesystem::create_directory(outside);
    std::filesystem::create_directory_symlink(outside, fixture.root / "redirect");
    CHECK(fixture.files->WriteDurable(fixture.root / "redirect" / "escape", {}).HasError());
    CHECK_FALSE(std::filesystem::exists(outside / "escape"));
    REQUIRE(fixture.files->InspectEntry(fixture.root / "redirect").Value()->kind == FileEntryKind::Redirect);
    std::filesystem::create_hard_link(fixture.root / "owned", fixture.root / "extra");
    CHECK(fixture.files->ReadFileBounded(fixture.root / "owned", 1024).HasError());
    CHECK(fixture.files->RemoveDurable(fixture.root / "extra").HasError());
    CHECK(mkfifo((fixture.root / "fifo").c_str(), 0600) == 0);
    CHECK(fixture.files->ReadFileBounded(fixture.root / "fifo", 1024).HasError());
    CHECK(fixture.files->InspectEntry(fixture.root / "fifo").Value()->kind == FileEntryKind::Other);
}
#endif
