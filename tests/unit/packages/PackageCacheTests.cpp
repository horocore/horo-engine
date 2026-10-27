#include "Horo/Packages/PackageCache.h"
#include "PackageArchiveTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace {
    using Horo::Packages::PackageCacheStore;
    using Horo::Packages::PackageQuarantineReason;
    using Horo::Packages::ValidatedPackageArchive;
    using Horo::Tests::Packages::TemporaryDirectory;

    [[nodiscard]] ValidatedPackageArchive VerifiedArchive() {
        auto result = ValidatedPackageArchive::Verify(Horo::Tests::Packages::ValidPackageArchiveBytes());
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    [[nodiscard]] std::string DigestHex(const Horo::Sha256Digest &digest) {
        return Horo::FormatSha256(digest).substr(7);
    }

    [[nodiscard]] std::filesystem::path ActivePath(const std::filesystem::path &root, const Horo::Sha256Digest &digest) {
        return root / "by-hash" / "sha256" / DigestHex(digest) / "archive.horopkg";
    }

    void CheckOwnerReadOnly(const std::filesystem::path &path) {
        const auto permissions = std::filesystem::status(path).permissions();
        CHECK((permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
        CHECK((permissions & (std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                              std::filesystem::perms::others_write)) == std::filesystem::perms::none);
#ifndef _WIN32
        CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    }

    const Horo::ErrorCodeDescriptor InjectedPermissionFailure{
        .domain = Horo::ErrorDomainId{"test.package-cache"},
        .code = Horo::ErrorCode{"test.permission_denied"},
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Injected permission failure.",
    };

    class DenyingFileSystem final : public Horo::DurableFileSystem {
    public:
        enum class Failure {
            None,
            Write,
            Replace
        };

        DenyingFileSystem(Horo::NativeDurableFileSystem &native, const Failure failure) : native_(native), failure_(failure) {}

        Horo::Result<bool> CreateDirectoryPath(const std::filesystem::path &path, Horo::DirectoryCreationMode mode) override {
            return native_.CreateDirectoryPath(path, mode);
        }

        Horo::Result<Horo::ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                  const std::string_view owner) override {
            return native_.TryAcquireExclusive(path, owner);
        }

        Horo::Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native_.AvailableBytes(path);
        }

        Horo::Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            if (failure_ == Failure::Write)
                return Horo::Result<void>::Failure(Horo::MakeError(InjectedPermissionFailure));
            return native_.WriteDurable(path, bytes);
        }

        std::function<void(const std::filesystem::path &)> beforePrivateWrite;
        std::vector<std::filesystem::path> privateWrites;

        Horo::Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                               const std::filesystem::perms permissions) override {
            privateWrites.push_back(path);
            if (beforePrivateWrite)
                beforePrivateWrite(path);
            if (failure_ == Failure::Write)
                return Horo::Result<void>::Failure(Horo::MakeError(InjectedPermissionFailure));
            return native_.WritePrivateDurable(path, bytes, created, permissions);
        }

        Horo::Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native_.CopyDurable(source, destination);
        }

        Horo::Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            if (failure_ == Failure::Replace)
                return Horo::Result<void>::Failure(Horo::MakeError(InjectedPermissionFailure));
            return native_.AtomicReplace(prepared, destination);
        }

        Horo::Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native_.RemoveDurable(path);
        }

        Horo::Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native_.SyncDirectory(path);
        }

    private:
        Horo::NativeDurableFileSystem &native_;
        Failure failure_;
    };

    [[nodiscard]] PackageCacheStore CreateStore(Horo::DurableFileSystem &files, const std::filesystem::path &root,
                                                const Horo::Packages::PackageValidationLimits &limits = {}) {
        auto result = PackageCacheStore::Create(files, root, limits);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    class CacheFixture final {
    public:
        CacheFixture() : store(CreateStore(files, temporary.Path())) {}

        TemporaryDirectory temporary;
        Horo::NativeDurableFileSystem files;
        PackageCacheStore store;
    };

    void CheckOwnerPrivate(const std::filesystem::path &path) {
        const auto permissions = std::filesystem::status(path).permissions();
        CHECK((permissions & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
        CHECK((permissions & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
#ifndef _WIN32
        CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    }

    Horo::Security::DetachedSignatureEnvelope Envelope(const ValidatedPackageArchive &archive) {
        return {.publisherId = "com.horo.tests",
                .keyId = "cache-test",
                .artifactDigest = archive.Digest(),
                .signature = std::vector<std::byte>(64, std::byte{7})};
    }

    std::filesystem::path EvidencePath(const CacheFixture &fixture, const Horo::Sha256Digest &digest) {
        return ActivePath(fixture.temporary.Path(), digest).parent_path() / "publisher-envelope.bin";
    }

    std::string EvidenceBytes(const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    TEST_CASE("Package cache atomically publishes, reverifies, snapshots and cleans entries", "[packages][cache]") {
        CacheFixture fixture;
        auto archive = VerifiedArchive();
        const auto digest = archive.Digest();

        const auto first = fixture.store.Publish(archive);
        REQUIRE(first.HasValue());
        CHECK_FALSE(first.Value().alreadyPresent);
        CHECK(first.Value().digest == digest);
        CheckOwnerReadOnly(ActivePath(fixture.temporary.Path(), digest));

        const auto second = fixture.store.Publish(archive);
        REQUIRE(second.HasValue());
        CHECK(second.Value().alreadyPresent);
        auto loaded = fixture.store.Load(digest);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().has_value());
        CHECK(loaded.Value()->Digest() == digest);

        REQUIRE(fixture.store.Remove(digest).Value());
        CHECK_FALSE(std::filesystem::exists(ActivePath(fixture.temporary.Path(), digest)));
        auto missing = fixture.store.Load(digest);
        REQUIRE(missing.HasValue());
        CHECK_FALSE(missing.Value().has_value());
        CHECK_FALSE(fixture.store.Remove(digest).Value());
    }

    TEST_CASE("Package cache quarantines a poisoned hit with safe persisted diagnostics", "[packages][cache][quarantine]") {
        CacheFixture fixture;
        auto archive = VerifiedArchive();
        const auto digest = archive.Digest();
        REQUIRE(fixture.store.Publish(archive).HasValue());

        const auto path = ActivePath(fixture.temporary.Path(), digest);
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(stream);
        stream.seekp(0);
        stream.put('x');
        stream.close();

        const auto loaded = fixture.store.Load(digest);
        REQUIRE(loaded.HasError());
        CHECK(loaded.ErrorValue().code.Value() == "packages.cache.corrupt");
        CHECK_FALSE(std::filesystem::exists(path));
        const auto quarantineRoot = fixture.temporary.Path() / "quarantine" / "corrupt-cache-entry";
        REQUIRE(std::filesystem::is_directory(quarantineRoot));
        const std::filesystem::directory_iterator records{quarantineRoot};
        const auto recordDirectory = records->path();
        CHECK(std::filesystem::is_regular_file(recordDirectory / "artifact.horopkg"));
        std::ifstream diagnostic{recordDirectory / "diagnostic.json"};
        const nlohmann::json document = nlohmann::json::parse(diagnostic);
        CHECK(document["reason"] == "corrupt-cache-entry");
        CHECK(document["expectedDigest"].is_string());
        CHECK(document["expectedDigest"] == Horo::FormatSha256(digest));
        CHECK(document["actualDigest"].is_string());
        CHECK(document["actualDigest"] != document["expectedDigest"]);
    }

    TEST_CASE("Package cache isolates failed downloads without publishing them", "[packages][cache][quarantine]") {
        CacheFixture fixture;
        const auto bytes = Horo::Tests::Packages::ValidPackageArchiveBytes();
        const auto expected = Horo::ComputeSha256({});

        const auto result = fixture.store.Quarantine(bytes, PackageQuarantineReason::HashMismatch, expected);
        REQUIRE(result.HasValue());
        CHECK(result.Value().reason == PackageQuarantineReason::HashMismatch);
        CHECK(result.Value().expectedDigest == expected);
        CHECK(result.Value().actualDigest == Horo::ComputeSha256(bytes));
        CHECK_FALSE(std::filesystem::exists(ActivePath(fixture.temporary.Path(), expected)));
        const auto artifact = fixture.temporary.Path() / "quarantine" / "hash-mismatch" / result.Value().quarantineId / "artifact.horopkg";
        CHECK(std::filesystem::is_regular_file(artifact));
        CheckOwnerReadOnly(artifact);

        const auto withoutExpected = fixture.store.Quarantine(bytes, PackageQuarantineReason::VerificationFailure);
        REQUIRE(withoutExpected.HasValue());
        const auto diagnosticPath =
            fixture.temporary.Path() / "quarantine" / "verification-failure" / withoutExpected.Value().quarantineId / "diagnostic.json";
        std::ifstream diagnostic{diagnosticPath};
        const nlohmann::json document = nlohmann::json::parse(diagnostic);
        CHECK(document["expectedDigest"].is_null());
        CHECK(document["actualDigest"].is_string());
    }

    TEST_CASE("Package cache returns busy while publication or cleanup owns the digest lock", "[packages][cache][concurrency]") {
        CacheFixture fixture;
        auto archive = VerifiedArchive();
        const auto lockPath = fixture.temporary.Path() / "locks" / (DigestHex(archive.Digest()) + ".lock");
        auto held = fixture.files.TryAcquireExclusive(lockPath, "test-owner");
        REQUIRE(held.HasValue());

        const auto publish = fixture.store.Publish(archive);
        REQUIRE(publish.HasError());
        CHECK(publish.ErrorValue().code.Value() == "packages.cache.busy");
        const auto remove = fixture.store.Remove(archive.Digest());
        REQUIRE(remove.HasError());
        CHECK(remove.ErrorValue().code.Value() == "packages.cache.busy");
    }

    TEST_CASE("Package cache preserves the active namespace on permission and atomic replacement failures", "[packages][cache]") {
        const auto archive = VerifiedArchive();
        for (const auto failure : {DenyingFileSystem::Failure::Write, DenyingFileSystem::Failure::Replace}) {
            TemporaryDirectory temporary;
            Horo::NativeDurableFileSystem native;
            DenyingFileSystem files{native, failure};
            auto store = CreateStore(files, temporary.Path());
            const auto result = store.Publish(archive);
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "packages.cache.io_failed");
            REQUIRE(result.ErrorValue().cause);
            CHECK(result.ErrorValue().cause.Get()->code.Value() == "test.permission_denied");
            CHECK_FALSE(std::filesystem::exists(ActivePath(temporary.Path(), archive.Digest())));
        }
    }

    TEST_CASE("Package cache bounds quarantine input and rejects noncanonical roots", "[packages][cache]") {
        TemporaryDirectory temporary;
        Horo::NativeDurableFileSystem files;
        CHECK(PackageCacheStore::Create(files, std::filesystem::path{"relative/cache"}).HasError());
        auto store = CreateStore(files, temporary.Path(), {.archiveBytes = 1});
        const auto archive = VerifiedArchive();
        const auto publish = store.Publish(archive);
        REQUIRE(publish.HasError());
        CHECK(publish.ErrorValue().code.Value() == "packages.validation.limit");
        CHECK_FALSE(std::filesystem::exists(ActivePath(temporary.Path(), archive.Digest())));
        const std::array bytes{std::byte{1}, std::byte{2}};
        const auto result = store.Quarantine(bytes, PackageQuarantineReason::InvalidArchive);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "packages.cache.resource_limit");
    }

    TEST_CASE("Package cache persists inert detached evidence and removes it with its archive", "[packages][cache][publisher]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        const auto signature = Envelope(archive);
        REQUIRE(fixture.store.Publish(archive, signature).HasValue());
        CheckOwnerPrivate(EvidencePath(fixture, archive.Digest()));
        auto loaded = fixture.store.LoadWithPublisherEvidence(archive.Digest());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value());
        REQUIRE(loaded.Value()->signature);
        CHECK(loaded.Value()->signature->signature == signature.signature);
        CHECK(loaded.Value()->signature->publisherId == signature.publisherId);
        CHECK(loaded.Value()->archive.Digest() == archive.Digest());
        REQUIRE(fixture.store.Remove(archive.Digest()).HasValue());
        CHECK_FALSE(std::filesystem::exists(EvidencePath(fixture, archive.Digest())));
        CHECK(loaded.Value()->archive.Digest() == archive.Digest());  // Owned snapshot survives removal.
    }

    TEST_CASE("Package cache rejects malformed versioned publisher storage before returning evidence", "[packages][cache][security]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        REQUIRE(fixture.store.Publish(archive, Envelope(archive)).HasValue());
        const auto path = EvidencePath(fixture, archive.Digest());
        auto bytes = EvidenceBytes(path);
        const auto variant = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
        switch (variant) {
            case 0:
                bytes.pop_back();
                break;
            case 1:
                bytes.push_back('x');
                break;
            case 2:
                bytes[5] = 2;
                break;  // Unknown storage version.
            case 3:
                bytes[8] = 2;
                break;  // Unknown algorithm.
            case 4:
                bytes[47] = 0;
                break;  // Invalid required publisher identity.
            case 5:
                bytes[15] ^= 1;
                break;  // Differently bound archive digest.
            case 6:
                bytes[9] = bytes[10] = static_cast<char>(0xff);
                break;
            case 7:
                bytes.resize(2049);
                break;
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        CHECK(fixture.store.LoadWithPublisherEvidence(archive.Digest()).HasError());
        CHECK(fixture.store.Load(archive.Digest()).HasValue());  // Legacy unsigned archive API is unchanged.
    }

    TEST_CASE("Package cache retains previous publisher bytes when evidence writes fail", "[packages][cache][atomic]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        auto signature = Envelope(archive);
        REQUIRE(fixture.store.Publish(archive, signature).HasValue());
        const auto original = EvidenceBytes(EvidencePath(fixture, archive.Digest()));
        signature.keyId = "rotated-key";
        const auto failure = GENERATE(DenyingFileSystem::Failure::Write, DenyingFileSystem::Failure::Replace);
        DenyingFileSystem denied{fixture.files, failure};
        auto store = CreateStore(denied, fixture.temporary.Path());
        CHECK(store.Publish(archive, signature).HasError());
        CHECK(EvidenceBytes(EvidencePath(fixture, archive.Digest())) == original);
        CheckOwnerPrivate(EvidencePath(fixture, archive.Digest()));
        REQUIRE(denied.privateWrites.size() == 1);
        CHECK_FALSE(std::filesystem::exists(denied.privateWrites.front()));
    }

    TEST_CASE("Package cache publisher operations reject hostile evidence files and redirects", "[packages][cache][security]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        REQUIRE(fixture.store.Publish(archive).HasValue());
        const auto path = EvidencePath(fixture, archive.Digest());
        SECTION("directory entry") {
            std::filesystem::create_directory(path);
            CHECK(fixture.store.Publish(archive, Envelope(archive)).HasError());
            CHECK(fixture.store.LoadWithPublisherEvidence(archive.Digest()).HasError());
        }
        SECTION("redirect") {
            const auto target = fixture.temporary.Path() / "preserve";
            {
                std::ofstream output{target};
                output << "unchanged";
            }
            std::error_code error;
            std::filesystem::create_symlink(target, path, error);
            if (error)
                SKIP("Host cannot create the requested symbolic-link fixture: " << error.message());
            CHECK(fixture.store.Publish(archive, Envelope(archive)).HasError());
            CHECK(fixture.store.LoadWithPublisherEvidence(archive.Digest()).HasError());
            CHECK(EvidenceBytes(target) == "unchanged");
        }
    }

    TEST_CASE("Package cache quarantining corrupt archive bytes also retires detached evidence", "[packages][cache][quarantine]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        REQUIRE(fixture.store.Publish(archive, Envelope(archive)).HasValue());
        const auto path = ActivePath(fixture.temporary.Path(), archive.Digest());
        std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            output << "poisoned";
        }
        CHECK(fixture.store.LoadWithPublisherEvidence(archive.Digest()).HasError());
        CHECK_FALSE(std::filesystem::exists(path));
        CHECK_FALSE(std::filesystem::exists(EvidencePath(fixture, archive.Digest())));
    }

    TEST_CASE("Package cache exclusive archive and envelope stages preserve preexisting hard-link victims", "[packages][cache][security]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        const bool publisherStage = GENERATE(false, true);
        if (publisherStage)
            REQUIRE(fixture.store.Publish(archive).HasValue());
        const auto victim = fixture.temporary.Path() / "victim";
        {
            std::ofstream output{victim};
            output << "preserve victim bytes";
        }
        const auto originalPermissions = std::filesystem::status(victim).permissions();
        DenyingFileSystem attacked{fixture.files, DenyingFileSystem::Failure::None};
        std::optional<std::filesystem::path> linkedStage;
        attacked.beforePrivateWrite = [&](const std::filesystem::path &path) {
            if (path.filename().string().find(publisherStage ? ".publisher." : ".archive.") == std::string::npos)
                return;
            std::filesystem::create_hard_link(victim, path);
            linkedStage = path;
        };
        auto store = CreateStore(attacked, fixture.temporary.Path());
        CHECK(store.Publish(archive, Envelope(archive)).HasError());
        REQUIRE(linkedStage);
        CHECK(EvidenceBytes(victim) == "preserve victim bytes");
        CHECK(std::filesystem::status(victim).permissions() == originalPermissions);
        CHECK(std::filesystem::equivalent(*linkedStage, victim));  // Unowned preexisting paths are not cleaned up.
    }

    TEST_CASE("Package cache replacement and corrupt-entry quarantine do not mutate linked destination victims",
              "[packages][cache][security]") {
        CacheFixture fixture;
        const auto archive = VerifiedArchive();
        const bool publisherEntry = GENERATE(false, true);
        if (publisherEntry)
            REQUIRE(fixture.store.Publish(archive).HasValue());
        const auto victim = fixture.temporary.Path() / "destination-victim";
        {
            std::ofstream output{victim};
            output << "preserve destination bytes";
        }
        const auto permissions = std::filesystem::status(victim).permissions();
        const auto entry =
            publisherEntry ? EvidencePath(fixture, archive.Digest()) : ActivePath(fixture.temporary.Path(), archive.Digest());
        std::filesystem::create_directories(entry.parent_path());
        std::filesystem::create_hard_link(victim, entry);
        REQUIRE(fixture.store.Publish(archive, Envelope(archive)).HasValue());
        CHECK(EvidenceBytes(victim) == "preserve destination bytes");
        CHECK(std::filesystem::status(victim).permissions() == permissions);
        CHECK_FALSE(std::filesystem::equivalent(victim, entry));
        CheckOwnerReadOnly(ActivePath(fixture.temporary.Path(), archive.Digest()));
    }
}  // namespace
