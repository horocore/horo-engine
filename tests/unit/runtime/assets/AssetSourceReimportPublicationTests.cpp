#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace Horo;
using namespace Horo::Assets;

namespace {
    struct ReimportSourceProject final {
        Tests::OwnedTestDirectory directory{"reimport-source-policy"};
        NativeDurableFileSystem files;
        std::filesystem::path source{directory.Path() / "assets/source.bin"};

        ReimportSourceProject() {
            std::filesystem::create_directories(source.parent_path());
        }

        void Sparse(std::size_t size) const {
            std::ofstream stream(source, std::ios::binary);
            REQUIRE(stream.good());
            stream.seekp(static_cast<std::streamoff>(size - 1));
            stream.put('\0');
            stream.close();
            REQUIRE(stream.good());
            REQUIRE(std::filesystem::file_size(source) == size);
        }

        ExclusiveFileLock Lease() {
            const auto path = AssetSourcePublicationLockPath(directory.Path(), source);
            REQUIRE(path.HasValue());
            auto lease = files.TryAcquireExclusive(path.Value(), "reimport-source-policy-test");
            REQUIRE(lease.HasValue());
            return std::move(lease).Value();
        }
    };

    /** @brief Captures the actual sealed native stage before its tracked canonical replacement. */
    class NativeAttemptSnapshot final : public TestSupport::NativePublicationFiles {
    public:
        std::filesystem::path live;
        std::filesystem::path restarted;
        std::filesystem::path observed;

        Result<void> WritePrivateDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes, bool &created,
                                         const std::filesystem::perms permissions) override {
            auto written = native.WritePrivateDurable(path, bytes, created, permissions);
            if (written.HasValue() && path.filename() == "payload.tmp") {
                observed = restarted / path.lexically_relative(live);
                std::filesystem::create_directories(observed.parent_path());
                for (const auto &file : std::filesystem::directory_iterator(path.parent_path()))
                    std::filesystem::copy_file(file.path(), observed.parent_path() / file.path().filename());
            }
            return written;
        }
    };
}  // namespace

TEST_CASE("Asset source capture retains existing identity under long Unicode native project roots",
          "[assets][reimport-source][long-path]") {
    Tests::OwnedTestDirectory directory{"source-long-path"};
    NativeDurableFileSystem files;
    std::vector<std::filesystem::path> directories{directory.Path()};
    for (unsigned index = 0; index < 4; ++index) {
        directories.push_back(directories.back() / (std::string(72, 'c') + std::to_string(index)));
        const auto created = files.CreateDirectoryPath(directories.back(), DirectoryCreationMode::Single);
        REQUIRE(created.HasValue());
        REQUIRE(created.Value());
    }
    const auto &root = directories.back();
    const auto source = root / std::filesystem::path{u8"source \u00fc asset.bin"};
    REQUIRE(root.native().size() > 260);
    const std::string payload{"retained source bytes"};
    const auto bytes = std::as_bytes(std::span{payload});
    REQUIRE(files.WriteDurable(source, bytes).HasValue());
    const auto captured = CaptureAssetSource(root, source, 1024);
    INFO((captured.HasError() ? captured.ErrorValue().message : "Long source captured"));
    REQUIRE(captured.HasValue());
    CHECK(captured.Value().identity.exists);
    CHECK(captured.Value().identity.bytes == payload.size());
    CHECK(captured.Value().identity.digest == ComputeSha256(bytes));
    CHECK(captured.Value().bytes == std::vector<std::byte>(bytes.begin(), bytes.end()));
    const auto inspected = InspectReimportAssetSource(root, source);
    REQUIRE(inspected.HasValue());
    CHECK(inspected.Value() == captured.Value().identity);
    CHECK(CaptureAssetSource(root, source, payload.size() - 1).HasError());
    const auto missing = CaptureAssetSource(root, root / "absent.bin", 1024);
    REQUIRE(missing.HasValue());
    CHECK_FALSE(missing.Value().identity.exists);
    REQUIRE(files.RemoveDurable(source).HasValue());
    for (auto path = directories.rbegin(); path != directories.rend(); ++path)
        REQUIRE(files.RemoveDurable(*path).HasValue());
}

TEST_CASE("Reimport identity streaming preserves missing and existing-empty distinctions", "[assets][reimport-source]") {
    ReimportSourceProject project;
    const auto missing = InspectReimportAssetSource(project.directory.Path(), project.source);
    REQUIRE(missing.HasValue());
    CHECK_FALSE(missing.Value().exists);
    REQUIRE(project.files.WriteDurable(project.source, {}).HasValue());
    const auto empty = InspectReimportAssetSource(project.directory.Path(), project.source);
    const auto owned = CaptureReimportAssetSource(project.directory.Path(), project.source);
    REQUIRE(empty.HasValue());
    REQUIRE(owned.HasValue());
    CHECK(empty.Value().exists);
    CHECK(empty.Value().bytes == 0);
    CHECK(empty.Value() == owned.Value().identity);
    CHECK(empty.Value().digest == ComputeSha256(std::span<const std::byte>{}));
    CHECK(empty.Value() != missing.Value());
}

TEST_CASE("Explicit reimport policy retains legacy payloads without widening default source publication", "[assets][reimport-source]") {
    ReimportSourceProject project;
    constexpr std::size_t MiB = 1024 * 1024;
    project.Sparse(65 * MiB);
    const auto ordinary = CaptureAssetSource(project.directory.Path(), project.source, 64 * MiB);
    REQUIRE(ordinary.HasError());
    CHECK(ordinary.ErrorValue().code.Value() == AssetSourceErrors::Limit.code.Value());
    AssetSourceIdentity accepted;
    {
        const auto captured = CaptureReimportAssetSource(project.directory.Path(), project.source);
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value().bytes.size() == 65 * MiB);
        accepted = captured.Value().identity;
        CHECK(accepted.digest == ComputeSha256(captured.Value().bytes));
        const auto streamed = InspectReimportAssetSource(project.directory.Path(), project.source);
        REQUIRE(streamed.HasValue());
        CHECK(streamed.Value() == accepted);
    }  // Release the original large capture before staging or importing anything else.
    auto lease = project.Lease();
    const std::array replacement{std::byte{0x42}};
    const auto narrow = PublishAssetSource(project.directory.Path(), project.source, accepted, replacement, lease, project.files);
    REQUIRE(narrow.HasError());
    CHECK(narrow.ErrorValue().code.Value() == AssetSourceErrors::Limit.code.Value());
    CHECK(std::filesystem::file_size(project.source) == 65 * MiB);
    const auto published =
        PublishReimportAssetSource(project.directory.Path(), project.source, accepted, replacement, lease, project.files);
    REQUIRE(published.HasValue());
    CHECK(published.Value().status == AssetSourcePublicationStatus::Durable);
    const auto current = CaptureAssetSource(project.directory.Path(), project.source, 1024);
    REQUIRE(current.HasValue());
    CHECK(current.Value().bytes == std::vector<std::byte>(replacement.begin(), replacement.end()));
    CHECK(current.Value().identity == published.Value().identity);
}

TEST_CASE("Reimport source ceiling and cancellation reject before reading or allocating a large payload", "[assets][reimport-source]") {
    ReimportSourceProject project;
    project.Sparse(1024ULL * 1024 * 1024 + 1);
    const auto streamed = InspectReimportAssetSource(project.directory.Path(), project.source);
    const auto owned = CaptureReimportAssetSource(project.directory.Path(), project.source);
    REQUIRE(streamed.HasError());
    REQUIRE(owned.HasError());
    CHECK(streamed.ErrorValue().code.Value() == AssetSourceErrors::Limit.code.Value());
    CHECK(owned.ErrorValue().code.Value() == AssetSourceErrors::Limit.code.Value());
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto cancelled = InspectReimportAssetSource(project.directory.Path(), project.source, cancellation.Token());
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == AssetSourceErrors::Cancelled.code.Value());
}

TEST_CASE("Captured import metadata decodes the admitted generation with the existing finite schema", "[assets][reimport-source]") {
    const auto decoded =
        DecodeAssetImportMetadata(R"({"schemaVersion":1,"assetId":"00000000-0000-0000-0000-000000000081","assetType":"core.terrain"})");
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().assetId == AssetId::Parse("00000000-0000-0000-0000-000000000081").Value());
    CHECK(decoded.Value().assetType.Value() == "core.terrain");
    CHECK(decoded.Value().importerContributionId.empty());
    CHECK(DecodeAssetImportMetadata("").HasError());
    CHECK(DecodeAssetImportMetadata("{invalid").HasError());
    CHECK(DecodeAssetImportMetadata(std::string(1024 * 1024 + 1, ' ')).HasError());
    CHECK(DecodeAssetImportMetadata(R"({"schemaVersion":2})").HasError());
}

namespace {
    /** @brief Reopens a native stage captured before the canonical replacement. */
    struct RestartableSourceFixture final {
        const std::array<std::byte, 2> original{std::byte{0x01}, std::byte{0x02}};
        const std::array<std::byte, 3> planned{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}};
        const AssetSourceIdentity accepted{true, original.size(), ComputeSha256(original)};
        const AssetSourcePublicationAttempt attempt{AssetId::Parse("00000000-0000-0000-0000-000000000081").Value(),
                                                    {true, planned.size(), ComputeSha256(planned)}};
        ReimportSourceProject live;
        ReimportSourceProject restarted;
        NativeAttemptSnapshot snapshot;
        ExclusiveFileLock restartedLease{restarted.Lease()};

        RestartableSourceFixture() {
            REQUIRE(live.files.WriteDurable(live.source, original).HasValue());
            REQUIRE(restarted.files.WriteDurable(restarted.source, original).HasValue());
            snapshot.live = live.directory.Path();
            snapshot.restarted = restarted.directory.Path();
            auto lease = live.Lease();
            REQUIRE(PublishAssetSourceAttempt(snapshot.live, live.source, accepted, planned, attempt, lease, snapshot).HasValue());
            REQUIRE_FALSE(snapshot.observed.empty());
            REQUIRE(std::filesystem::is_regular_file(snapshot.observed));
        }

        Result<void> Retire(const AssetSourcePublicationAttempt &candidate) {
            return RetireAssetSourcePublicationAttempt(snapshot.restarted, restarted.source, candidate, restartedLease, restarted.files);
        }
    };
}  // namespace

TEST_CASE("Restartable asset source attempts retire exact sealed stages and preserve foreign evidence", "[assets][source-attempt]") {
    RestartableSourceFixture fixture;
    SECTION("the complete observed stage can be retired without canonical replacement") {
        REQUIRE(fixture.Retire(fixture.attempt).HasValue());
        CHECK_FALSE(std::filesystem::exists(fixture.snapshot.observed.parent_path()));
        REQUIRE(fixture.Retire(fixture.attempt).HasValue());
        CHECK(CaptureAssetSource(fixture.snapshot.restarted, fixture.restarted.source, 1024).Value().identity == fixture.accepted);
    }
    SECTION("a stale seal binding is denied") {
        auto stale = fixture.attempt;
        ++stale.payload.bytes;
        REQUIRE(fixture.Retire(stale).HasError());
        CHECK(std::filesystem::is_regular_file(fixture.snapshot.observed));
    }
    SECTION("an interrupted partial stage is preserved and cannot block a fresh attempt") {
        const auto partial = std::span{fixture.planned}.first(1);
        REQUIRE(fixture.restarted.files.WriteDurable(fixture.snapshot.observed, partial).HasValue());
        REQUIRE(fixture.Retire(fixture.attempt).HasError());
        const auto nextId = AssetId::Parse("00000000-0000-0000-0000-000000000082").Value();
        const AssetSourcePublicationAttempt next{nextId, fixture.attempt.payload};
        const auto result = PublishAssetSourceAttempt(fixture.snapshot.restarted, fixture.restarted.source, fixture.accepted,
                                                      fixture.planned, next, fixture.restartedLease, fixture.restarted.files);
        REQUIRE(result.HasValue());
        CHECK(result.Value().status == AssetSourcePublicationStatus::Durable);
        REQUIRE(fixture.Retire(next).HasValue());
        CHECK(CaptureAssetSource(fixture.snapshot.restarted, fixture.snapshot.observed, 1024).Value().identity.digest ==
              ComputeSha256(partial));
        CHECK(CaptureAssetSource(fixture.snapshot.restarted, fixture.restarted.source, 1024).Value().identity == fixture.attempt.payload);
    }
    SECTION("a stage with foreign siblings is neither reused nor retired") {
        const auto foreign = fixture.snapshot.observed.parent_path() / "unowned.tmp";
        REQUIRE(fixture.restarted.files.WriteDurable(foreign, fixture.original).HasValue());
        REQUIRE(PublishAssetSourceAttempt(fixture.snapshot.restarted, fixture.restarted.source, fixture.accepted, fixture.planned,
                                          fixture.attempt, fixture.restartedLease, fixture.restarted.files)
                    .HasError());
        REQUIRE(fixture.Retire(fixture.attempt).HasError());
        CHECK(std::filesystem::is_regular_file(foreign));
        CHECK(std::filesystem::is_regular_file(fixture.snapshot.observed));
    }
    SECTION("a multiply linked stage is never removed") {
        const auto alias = fixture.restarted.source.parent_path() / "another-link.tmp";
        std::error_code error;
        std::filesystem::create_hard_link(fixture.snapshot.observed, alias, error);
        if (error)
            SKIP("Host does not permit hard links.");
        REQUIRE(fixture.Retire(fixture.attempt).HasError());
        CHECK(std::filesystem::is_regular_file(alias));
        CHECK(std::filesystem::is_regular_file(fixture.snapshot.observed));
    }
}

TEST_CASE("Restartable asset source attempts preserve redirected stages", "[assets][source-attempt]") {
    RestartableSourceFixture fixture;
    REQUIRE(fixture.restarted.files.RemoveDurable(fixture.snapshot.observed).HasValue());
    std::error_code error;
    std::filesystem::create_symlink(fixture.restarted.source, fixture.snapshot.observed, error);
    if (error)
        SKIP("Host does not permit symlinks.");
    REQUIRE(fixture.Retire(fixture.attempt).HasError());
    CHECK(std::filesystem::is_symlink(fixture.snapshot.observed));
    CHECK(CaptureAssetSource(fixture.snapshot.restarted, fixture.restarted.source, 1024).Value().identity == fixture.accepted);
}
