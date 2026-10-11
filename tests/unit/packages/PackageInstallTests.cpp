#include "Horo/Packages/PackageInstall.h"
#include "Horo/Packages/PackageInstallErrors.h"
#include "PackageArchiveTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>

using namespace Horo;
using namespace Horo::Packages;
using Horo::Tests::Packages::TemporaryDirectory;

namespace {
    HoroPackageId PackageId(const std::string_view value) {
        auto id = HoroPackageId::Parse(value);
        REQUIRE(id.HasValue());
        return std::move(id).Value();
    }

    HoroPackageSourceId SourceId(const std::string_view value) {
        auto id = HoroPackageSourceId::Parse(value);
        REQUIRE(id.HasValue());
        return std::move(id).Value();
    }

    PackageVersion Version(const std::string_view value) {
        auto version = PackageVersion::Parse(value);
        REQUIRE(version.HasValue());
        return std::move(version).Value();
    }

    std::shared_ptr<PackageRestoreGraph> Graph() {
        auto bytes = Horo::Tests::Packages::ValidPackageArchiveBytes();
        auto verified = ValidatedPackageArchive::Verify(bytes);
        REQUIRE(verified.HasValue());
        auto archive = std::make_shared<const ValidatedPackageArchive>(std::move(verified).Value());
        LockedPackage lock{.package = PackageId("com.horo.install-fixture"),
                           .version = Version("1.0.0"),
                           .source = SourceId("fixture.source"),
                           .artifactDigest = archive->Digest(),
                           .manifestDigest = archive->PackageManifestDigest(),
                           .fileManifestDigest = archive->Manifest().Digest()};
        PackageRestorePackage package{std::move(lock), archive, false};
        return std::make_shared<PackageRestoreGraph>(PackageRestoreGraph{ComputeSha256(std::as_bytes(std::span{"install-request", 15U})),
                                                                         {"linux", "x64", "horo-sdk-2"},
                                                                         {std::move(package)}});
    }

    std::string ReadFile(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    class FaultyFiles final : public DurableFileSystem {
    public:
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    const std::string_view owner) override {
            if (failLock)
                return Result<ExclusiveFileLock>::Failure(MakeError(PackageInstallErrors::LockUnavailable));
            return native.TryAcquireExclusive(path, owner);
        }

        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override {
            return native.AvailableBytes(path);
        }

        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            if (verifyLease) {
                auto contender = native.TryAcquireExclusive(path.parent_path() / "packages.install.lock", "install test contender");
                leaseRetained = contender.HasError();
                if (!leaseRetained)
                    return Result<void>::Failure(MakeError(PackageInstallErrors::CommitFailed));
            }
            if (failWrite)
                return Result<void>::Failure(MakeError(PackageInstallErrors::CommitFailed));
            return native.WriteDurable(path, bytes);
        }

        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override {
            return native.CopyDurable(source, destination);
        }

        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override {
            if (failReplace)
                return Result<void>::Failure(MakeError(PackageInstallErrors::CommitFailed));
            return native.AtomicReplace(prepared, destination);
        }

        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override {
            return native.RemoveDurable(path);
        }

        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override {
            return native.SyncDirectory(path);
        }

        NativeDurableFileSystem native;
        bool failWrite{};
        bool failReplace{};
        bool failLock{};
        bool verifyLease{};
        bool leaseRetained{};
    };
}  // namespace

TEST_CASE("Package install atomically retains the previous record on invalid evidence and cancellation", "[packages][install]") {
    TemporaryDirectory project;
    NativeDurableFileSystem files;
    auto created = PackageInstallService::Create(files, std::filesystem::canonical(project.Path()));
    REQUIRE(created.HasValue());
    auto service = std::move(created).Value();
    auto first = Graph();
    CancellationSource running;

    REQUIRE(service.Install(first, running.Token()).HasValue());
    const auto committed = service.ActiveGraph();
    REQUIRE(committed);
    REQUIRE(committed != first);
    REQUIRE(committed->requestHash == first->requestHash);
    const auto recordPath = project.Path() / ".horo/packages.installed.json";
    const auto originalRecord = ReadFile(recordPath);
    CHECK(originalRecord.find("com.horo.install-fixture") != std::string::npos);

    auto invalid = Graph();
    invalid->packages.front().lock.artifactDigest = {};
    auto rejected = service.Install(invalid, running.Token());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PackageInstallErrors::EvidenceMismatch.code.Value());
    CHECK(service.ActiveGraph() == committed);
    CHECK(ReadFile(recordPath) == originalRecord);

    CancellationSource cancelled;
    cancelled.RequestCancellation();
    auto second = Graph();
    auto stopped = service.Install(second, cancelled.Token());
    REQUIRE(stopped.HasError());
    CHECK(stopped.ErrorValue().code.Value() == PackageInstallErrors::Cancelled.code.Value());
    CHECK(service.ActiveGraph() == committed);
    CHECK(ReadFile(recordPath) == originalRecord);
}

TEST_CASE("Package install leaves the previous record active when a durable commit fails", "[packages][install]") {
    TemporaryDirectory project;
    FaultyFiles files;
    auto created = PackageInstallService::Create(files, std::filesystem::canonical(project.Path()));
    REQUIRE(created.HasValue());
    auto service = std::move(created).Value();
    auto first = Graph();
    CancellationSource running;
    files.verifyLease = true;
    REQUIRE(service.Install(first, running.Token()).HasValue());
    REQUIRE(files.leaseRetained);
    const auto committed = service.ActiveGraph();
    const auto recordPath = project.Path() / ".horo/packages.installed.json";
    const auto originalRecord = ReadFile(recordPath);

    files.failWrite = true;
    CHECK(service.Install(Graph(), running.Token()).HasError());
    files.failWrite = false;
    files.failLock = true;
    CHECK(service.Install(Graph(), running.Token()).HasError());
    files.failLock = false;
    files.failReplace = true;
    CHECK(service.Install(Graph(), running.Token()).HasError());
    CHECK(service.ActiveGraph() == committed);
    CHECK(ReadFile(recordPath) == originalRecord);
    CHECK_FALSE(std::filesystem::exists(project.Path() / ".horo/packages.install.pending"));
}

TEST_CASE("Package install rejects noncanonical project roots and incomplete graphs", "[packages][install]") {
    TemporaryDirectory project;
    NativeDurableFileSystem files;
    CHECK(PackageInstallService::Create(files, "relative/project").HasError());
    auto created = PackageInstallService::Create(files, std::filesystem::canonical(project.Path()));
    REQUIRE(created.HasValue());
    auto service = std::move(created).Value();
    CancellationSource running;
    auto missing = Graph();
    missing->packages.front().archive.reset();
    CHECK(service.Install(missing, running.Token()).HasError());
    CHECK_FALSE(service.ActiveGraph());
    CHECK_FALSE(std::filesystem::exists(project.Path() / ".horo/packages.installed.json"));
}

TEST_CASE("Package install admits a fresh canonical project and rejects metadata files", "[packages][install]") {
    TemporaryDirectory project;
    NativeDurableFileSystem files;
    const auto root = std::filesystem::canonical(project.Path());
    REQUIRE_FALSE(std::filesystem::exists(root / ".horo"));
    CHECK(PackageInstallService::Create(files, root).HasValue());
    CHECK_FALSE(std::filesystem::exists(root / ".horo"));

    {
        std::ofstream metadata(root / ".horo");
        REQUIRE(metadata.good());
        metadata << "not a metadata directory";
    }
    auto rejected = PackageInstallService::Create(files, root);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == PackageInstallErrors::InvalidInput.code.Value());
}

#if !defined(_WIN32)
TEST_CASE("Package install rejects existing and dangling metadata symlinks", "[packages][install]") {
    TemporaryDirectory project;
    TemporaryDirectory external;
    NativeDurableFileSystem files;
    const auto root = std::filesystem::canonical(project.Path());
    const auto metadata = root / ".horo";
    std::filesystem::create_directory_symlink(external.Path(), metadata);
    CHECK(PackageInstallService::Create(files, root).HasError());
    std::filesystem::remove(metadata);
    std::filesystem::create_directory_symlink(external.Path() / "missing", metadata);
    CHECK(PackageInstallService::Create(files, root).HasError());
    CHECK(std::filesystem::is_empty(external.Path()));
}
#endif
