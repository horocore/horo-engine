#include "assets/AssetCookOutputFixture.h"

#if !defined(_WIN32)
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Horo;
using namespace Horo::Assets;
using namespace Horo::Assets::OutputTestSupport;

TEST_CASE("Filesystem failure before pointer commit preserves the prior generation and recoverable staging", "[native]") {
    using enum PublicationFault;
    const std::array faults = {ArtifactWrite, ManifestWrite, PointerWrite, GenerationRename, GenerationFlush, RootFlush, PointerRename};
    for (const auto fault : faults) {
        TempDir tmp;
        const auto id = Id("00000000-0000-0000-0000-000000000001");
        const auto oldBytes = MakePayload(id, 8, 1);
        const auto newBytes = MakePayload(id, 8, 2);
        PublicationFiles files;
        files.root = tmp.path;
        REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, oldBytes), oldBytes, 4096U, {},
                                               Policy(files, "10000000-0000-0000-0000-000000000001"))
                    .HasValue());
        const auto pointer = ReadText(tmp.path / "current.json");
        files.fault = fault;
        const auto result = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, newBytes), newBytes, 4096U, {},
                                                           Policy(files, "10000000-0000-0000-0000-000000000002"));
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "test.publication_io");
        CHECK(ReadText(tmp.path / "current.json") == pointer);
        CHECK(ResolveCurrentCookGeneration(tmp.path).HasValue());
        files.fault = None;
        auto lock = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
        REQUIRE(lock.HasValue());
        REQUIRE(RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lock.Value()})
                    .HasValue());
        CHECK(std::filesystem::is_empty(tmp.path / ".cook-staging"));
        CHECK(ReadText(tmp.path / "current.json") == pointer);
    }
}

TEST_CASE("Pointer durability failure and adapter exception retain true committed adoption", "[native]") {
    for (const auto fault :
         {PublicationFault::PointerSync, PublicationFault::PointerException, PublicationFault::PointerStandardException}) {
        TempDir tmp;
        const auto id = Id("00000000-0000-0000-0000-000000000001");
        const auto bytes = MakePayload(id, 8);
        PublicationFiles files;
        files.root = tmp.path;
        files.fault = fault;
        auto policy = Policy(files, "10000000-0000-0000-0000-000000000001");
        bool adopted = false;
        policy.afterCommit = [&adopted](const AssetCookGeneration &candidate) {
            adopted = true;
            CHECK(candidate.durabilityError.has_value());
        };
        const auto result = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy);
        REQUIRE(result.HasValue());
        CHECK(result.Value().durabilityError.has_value());
        CHECK(adopted);
        CHECK(ResolveCurrentCookGeneration(tmp.path).Value().manifestDigest == result.Value().manifestDigest);
    }
}

TEST_CASE("Standard and foreign filesystem exceptions before commit preserve the previous selector", "[native]") {
    for (const auto fault : {PublicationFault::GenerationException, PublicationFault::GenerationStandardException,
                             PublicationFault::PointerBeforeException, PublicationFault::PointerBeforeStandardException}) {
        TempDir tmp;
        const auto id = Id("00000000-0000-0000-0000-000000000001");
        const auto oldBytes = MakePayload(id, 8, 1);
        const auto newBytes = MakePayload(id, 8, 2);
        PublicationFiles files;
        files.root = tmp.path;
        REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, oldBytes), oldBytes, 4096U, {},
                                               Policy(files, "10000000-0000-0000-0000-000000000001"))
                    .HasValue());
        const auto pointer = ReadText(tmp.path / "current.json");
        files.fault = fault;
        bool adopted{};
        auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
        policy.afterCommit = [&adopted](const AssetCookGeneration &) {
            adopted = true;
        };
        const auto result =
            PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, newBytes), newBytes, 4096U, {}, policy);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "asset.cook.malformed_artifact");
        CHECK_FALSE(adopted);
        CHECK(ReadText(tmp.path / "current.json") == pointer);
        CHECK(ResolveCurrentCookGeneration(tmp.path).HasValue());
    }
}

TEST_CASE("Failed identical pointer replacement cannot masquerade as committed durability uncertainty", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    PublicationFiles files;
    files.root = tmp.path;
    REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                           Policy(files, "10000000-0000-0000-0000-000000000001"))
                .HasValue());
    files.fault = PublicationFault::PointerRename;
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                         Policy(files, "10000000-0000-0000-0000-000000000002"))
              .HasError());
}

TEST_CASE("Final cancellation preserves pointer and cancellation after commit preserves terminal truth", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    PublicationFiles files;
    files.root = tmp.path;
    REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                           Policy(files, "10000000-0000-0000-0000-000000000001"))
                .HasValue());
    const auto pointer = ReadText(tmp.path / "current.json");
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
    policy.beforeCommit = [] {
        return Result<void>::Failure(Error{.code = ErrorCode{"test.cancelled"}});
    };
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy).HasError());
    CHECK(ReadText(tmp.path / "current.json") == pointer);
    policy = Policy(files, "10000000-0000-0000-0000-000000000003");
    bool cancelled = false;
    policy.afterCommit = [&cancelled](const AssetCookGeneration &) {
        cancelled = true;
    };
    CHECK(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {}, policy).HasValue());
    CHECK(cancelled);
}

TEST_CASE("Writer contention rereads the then-current complete base and checkpoints freshness under lease", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    files.root = tmp.path;
    const auto first = Id("00000000-0000-0000-0000-000000000001");
    const auto second = Id("00000000-0000-0000-0000-000000000002");
    const auto firstBytes = MakePayload(first, 8, 1);
    const auto secondBytes = MakePayload(second, 8, 2);
    auto nativeLease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "competing writer");
    REQUIRE(nativeLease.HasValue());
    std::optional<ExclusiveFileLock> held{std::move(nativeLease).Value()};
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000002");
    std::size_t waits{};
    policy.waitingForWriter = [&waits, &held, &tmp, &first, &firstBytes, &files] {
        ++waits;
        held.reset();
        const auto competing = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(first, firstBytes), firstBytes,
                                                              4096U, {}, Policy(files, "10000000-0000-0000-0000-000000000001"));
        REQUIRE(competing.HasValue());
        return Result<void>::Success();
    };
    bool checked = false;
    policy.afterWriterAcquired = [&checked, &files, &tmp] {
        checked = true;
        CHECK(files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "checkpoint observer").HasError());
        return Result<void>::Success();
    };
    auto replacement =
        PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(second, secondBytes), secondBytes, 4096U, {}, policy);
    REQUIRE(replacement.HasValue());
    CHECK(waits == 1U);
    CHECK(checked);
    auto inventory = ReadCookGenerationContents(replacement.Value(), 4096U);
    REQUIRE(inventory.HasValue());
    REQUIRE(inventory.Value().entries.size() == 2U);
    CHECK(inventory.Value().artifacts.front() == firstBytes);
}

TEST_CASE("Restart cleans bounded owned staging without selecting inactive generations or repairing invalid authority", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    files.root = tmp.path;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    auto published =
        PublishFixture(tmp.path, Target("headless-null"), std::vector{Entry(id, bytes)}, std::vector<std::vector<std::uint8_t>>{bytes});
    REQUIRE(published.HasValue());
    const auto staging = tmp.path / ".cook-staging" / "20000000-0000-0000-0000-000000000001";
    std::filesystem::create_directories(staging / "generation");
    {
        std::ofstream output(staging / "generation" / (id.ToString() + ".cooked"));
        output << "interrupted";
    }
    {
        std::ofstream output(staging / "current.json");
        output << "interrupted";
    }
    SECTION("invalid current") {
        std::ofstream output(tmp.path / "current.json", std::ios::trunc);
        output << "invalid";
    }
    SECTION("missing established current") {
        std::filesystem::remove(tmp.path / "current.json");
    }
    auto lease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(
        RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasError());
    CHECK(std::filesystem::is_empty(tmp.path / ".cook-staging"));
    CHECK(std::filesystem::exists(published.Value().generationRoot));
    CHECK(ReadCookGenerationContents(published.Value(), 4096U).HasValue());
}

TEST_CASE("Recovery prevalidates all owned files and preserves unexpected paths and outside links", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    const auto operation = tmp.path / ".cook-staging" / "20000000-0000-0000-0000-000000000001";
    std::filesystem::create_directories(operation / "generation");
    const auto outside = tmp.path / "outside.txt";
    {
        std::ofstream output(outside);
        output << "protected";
    }
    const auto unexpected = operation / "generation" / "unexpected.txt";
    SECTION("unexpected file") {
        std::ofstream output(unexpected);
        output << "protected";
    }
    SECTION("symlink") {
        std::error_code error;
        std::filesystem::create_symlink(outside, operation / "current.json", error);
        if (error)
            SKIP("Host cannot create the symlink fixture.");
    }
    SECTION("hard link") {
        std::error_code error;
        std::filesystem::create_hard_link(outside, operation / "current.json", error);
        if (error)
            SKIP("Host cannot create the hard-link fixture.");
    }
    auto lease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(
        RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasError());
    CHECK(std::filesystem::exists(operation));
    CHECK(ReadText(outside) == "protected");
}

TEST_CASE("Native writer leases reject link aliases before truncating diagnostic metadata", "[native]") {
    TempDir tmp;
    NativeDurableFileSystem files;
    const auto outside = tmp.path / "outside.txt";
    {
        std::ofstream output(outside);
        output << "protected";
    }
    const auto lock = tmp.path / ".cook-writer.lock";
    std::error_code error;
    SECTION("symlink") {
        std::filesystem::create_symlink(outside, lock, error);
    }
    SECTION("hard link") {
        std::filesystem::create_hard_link(outside, lock, error);
    }
    if (error)
        SKIP("Host cannot create the lock alias fixture.");
    CHECK(files.TryAcquireExclusive(lock, "must not truncate").HasError());
    CHECK(ReadText(outside) == "protected");
}

TEST_CASE("Publication has no unlocked filesystem fallback", "[native]") {
    TempDir tmp;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    const std::vector entries{Entry(id, bytes)};
    const std::vector<std::vector<std::uint8_t>> payloads{bytes};
    CHECK(Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads).HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
    NativeDurableFileSystem files;
    const ExclusiveFileLock empty;
    CHECK(
        Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads, {},
                                            {.files = &files, .operationId = "10000000-0000-0000-0000-000000000001", .writerLease = &empty})
            .HasError());
    auto unrelated = files.TryAcquireExclusive(tmp.path / "unrelated.lock", "wrong authority");
    REQUIRE(unrelated.HasValue());
    CHECK(Horo::Assets::PublishCookGeneration(tmp.path, Target("headless-null"), entries, payloads, {},
                                              {.files = &files,
                                               .operationId = "10000000-0000-0000-0000-000000000001",
                                               .writerLease = &unrelated.Value()})
              .HasError());
    CHECK(RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &unrelated.Value()})
              .HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
}

TEST_CASE("First promotion cancellation retains a durable retryable unpublished selector", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto oldBytes = MakePayload(id, 8, 1);
    auto policy = Policy(files, "10000000-0000-0000-0000-000000000001");
    policy.beforeCommit = [&tmp] {
        REQUIRE_FALSE(std::filesystem::is_empty(tmp.path / "generations"));
        CHECK(ReadText(tmp.path / "current.json") == R"({"schemaVersion":2,"target":"headless-null","state":"unpublished"})");
        return Result<void>::Failure(Error{.code = ErrorCode{"test.cancelled"}});
    };
    const auto cancelled =
        PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, oldBytes), oldBytes, 4096U, {}, policy);
    REQUIRE(cancelled.HasError());
    REQUIRE(ResolveCurrentCookGeneration(tmp.path).HasError());
    CHECK(ResolveCurrentCookGeneration(tmp.path).ErrorValue().code.Value() == "asset.cook.not_published");
    const auto orphan = std::filesystem::directory_iterator(tmp.path / "generations")->path();
    {
        auto lease = files.native.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "restart");
        REQUIRE(lease.HasValue());
        auto recovery =
            RecoverCookPublication(tmp.path, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()});
        REQUIRE(recovery.HasValue());
        CHECK_FALSE(recovery.Value().has_value());
        CHECK(std::filesystem::is_empty(tmp.path / ".cook-staging"));
    }
    const auto newBytes = MakePayload(id, 8, 2);
    auto replacement = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, newBytes), newBytes, 4096U, {},
                                                      Policy(files, "10000000-0000-0000-0000-000000000002"));
    REQUIRE(replacement.HasValue());
    CHECK(std::filesystem::exists(orphan));
    CHECK(ReadCookGenerationContents(ResolveCurrentCookGeneration(tmp.path).Value(), 4096U).Value().artifacts.front() == newBytes);
}

TEST_CASE("Unconfirmed unpublished baseline durability aborts before first immutable promotion and safely retries", "[native]") {
    TempDir tmp;
    PublicationFiles files;
    files.fault = PublicationFault::BaselineSync;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    const auto result = PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                                       Policy(files, "10000000-0000-0000-0000-000000000001"));
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "test.publication_io");
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
    CHECK(ResolveCurrentCookGeneration(tmp.path).ErrorValue().code.Value() == "asset.cook.not_published");
    files.fault = PublicationFault::None;
    REQUIRE(PublishCookArtifactReplacement(tmp.path, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                           Policy(files, "10000000-0000-0000-0000-000000000002"))
                .HasValue());
}

TEST_CASE("Invalid empty target cannot create a selector or generation", "[native]") {
    TempDir tmp;
    CHECK(PublishFixture(tmp.path, AssetCookTargetId{}, {}, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(tmp.path / "current.json"));
    CHECK_FALSE(std::filesystem::exists(tmp.path / "generations"));
}

TEST_CASE("Native lease authority survives a move and rejects default and foreign paths", "[native]") {
    TempDir tmp;
    NativeDurableFileSystem files;
    const auto path = tmp.path / ".cook-writer.lock";
    auto acquired = files.TryAcquireExclusive(path, "owner");
    REQUIRE(acquired.HasValue());
    CHECK(acquired.Value().ProtectsPath(path));
    CHECK_FALSE(acquired.Value().ProtectsPath(tmp.path / "other.lock"));
    ExclusiveFileLock moved = std::move(acquired).Value();
    CHECK_FALSE(acquired.Value().ProtectsPath(path));
    CHECK(moved.ProtectsPath(path));
    CHECK_FALSE(ExclusiveFileLock{}.ProtectsPath(path));
}

TEST_CASE("Serialized publication and restart recovery support spaces and non-ASCII host roots", "[native]") {
    TempDir tmp;
    const auto root = tmp.path / std::filesystem::path{u8"nav output örnek"};
    std::filesystem::create_directory(root);
    PublicationFiles files;
    files.root = root;
    const auto id = Id("00000000-0000-0000-0000-000000000001");
    const auto bytes = MakePayload(id, 8);
    auto published = PublishCookArtifactReplacement(root, Target("headless-null"), Entry(id, bytes), bytes, 4096U, {},
                                                    Policy(files, "10000000-0000-0000-0000-000000000001"));
    REQUIRE(published.HasValue());
    CHECK(ResolveCurrentCookGeneration(root).Value().manifestDigest == published.Value().manifestDigest);
    auto lease = files.native.TryAcquireExclusive(root / ".cook-writer.lock", "restart");
    REQUIRE(lease.HasValue());
    CHECK(RecoverCookPublication(root, Target("headless-null"), 4096U, {}, {.files = &files, .writerLease = &lease.Value()}).HasValue());
    CHECK(std::filesystem::is_empty(root / ".cook-staging"));
}

#if !defined(_WIN32)
TEST_CASE("Writer contention is enforced by the OS across independent processes", "[native]") {
    TempDir tmp;
    std::array<int, 2> wake{};
    REQUIRE(pipe(wake.data()) == 0);
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        close(wake[1]);
        if (pollfd wakeSignal{.fd = wake[0], .events = POLLIN | POLLHUP, .revents = 0};
            poll(&wakeSignal, 1, 5000) != 1 || (wakeSignal.revents & POLLHUP) == 0)
            _exit(2);
        close(wake[0]);
        NativeDurableFileSystem childFiles;
        auto contested = childFiles.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "child");
        const bool busy = contested.HasError() && contested.ErrorValue().code.Value() == "filesystem.lock_busy";
        _exit(busy ? 0 : 3);
    }
    close(wake[0]);
    // Acquire after fork: the child registry is empty, so only the native OS lock can reject it.
    NativeDurableFileSystem files;
    auto lease = files.TryAcquireExclusive(tmp.path / ".cook-writer.lock", "parent");
    // Closing the writer releases the child only after the acquisition attempt, without a SIGPIPE race on timeout.
    close(wake[1]);
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(lease.HasValue());
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
#endif
