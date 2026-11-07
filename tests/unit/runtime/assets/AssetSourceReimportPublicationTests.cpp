#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetSourcePublication.h"
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
}  // namespace

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
