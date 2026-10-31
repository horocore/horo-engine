#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Platform.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"
#include "terrain/TerrainDocumentHost.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    const ErrorCodeDescriptor InjectedPublicationFailure{
        .domain = ErrorDomainId{"test.terrain"},
        .code = ErrorCode{"test.terrain.after_replace"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Injected failure after native replacement.",
    };

    class UncertainFiles final : public TestSupport::NativePublicationFiles {
    public:
        Result<void> WritePrivateDurable(const std::filesystem::path &p, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms mode) override {
            return native.WritePrivateDurable(p, bytes, created, mode);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &a, const std::filesystem::path &b,
                                          AtomicFileReplacementReceipt &receipt) override {
            auto replaced = native.AtomicReplaceTracked(a, b, receipt);
            if (replaced.HasValue() && receipt.WasCommitted())
                return Result<void>::Failure(MakeError(InjectedPublicationFailure));
            return replaced;
        }
    };

    class TerrainProject final {
    public:
        TerrainProject() : mutations(files) {
            root = directory.Path();
            std::filesystem::create_directories(root / "assets");
            path = root / "assets" / "terrain.horoasset";
            sidecar = path;
            sidecar += ".horo";
            std::vector<std::byte> zeros(9 * 9 * 4);
            REQUIRE(files.WriteDurable(path, zeros).HasValue());
            const auto contribution = TerrainRawImporterContribution();
            metadata.assetId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000001").Value();
            metadata.assetType = Assets::AssetTypeId::Parse("core.terrain").Value();
            metadata.importerContributionId = contribution.contributionId;
            metadata.importerVersion = contribution.version;
            metadata.importerPackageId = contribution.packageId;
            metadata.importerModuleId = contribution.moduleId;
            metadata.importerModuleVersion = contribution.moduleVersion;
            metadata.absoluteSourcePath = root / "input.terrainraw";
            metadata.sourceExtension = "terrainraw";
            metadata.sourceHash = Assets::HashAssetImportSource(std::vector<std::uint8_t>(zeros.size()));
            metadata.sourceByteSize = zeros.size();
            metadata.importSettings = {{"settings.terrain-width", "9"},     {"settings.terrain-height", "9"},
                                       {"settings.terrain-spacing-x", "1"}, {"settings.terrain-spacing-z", "1"},
                                       {"settings.terrain-origin-x", "0"},  {"settings.terrain-origin-z", "0"},
                                       {"settings.terrain-crs", ""}};
            metadata.lastImportReasons = {Assets::AssetImportReason::InitialImport};
            metadata.importedAtUtc = "2026-10-31T00:00:00.000Z";
            WriteMetadata();
        }

        TerrainProject(const TerrainProject &) = delete;
        TerrainProject &operator=(const TerrainProject &) = delete;

        void WriteMetadata() {
            const auto serialized = Assets::SerializeAssetImportMetadata(metadata);
            REQUIRE(serialized.HasValue());
            REQUIRE(files.WriteDurable(sidecar, std::as_bytes(std::span{serialized.Value()})).HasValue());
        }

        std::shared_ptr<TerrainDocumentHost> Open() {
            const auto id = registry.Open({DocumentKind::Asset, SourceDocumentId::Parse("assets/terrain.horoasset").Value()});
            REQUIRE(id.HasValue());
            auto opened = TerrainDocumentHost::Open(root, path, metadata.assetId, id.Value().identity, metadata.assetId, mutations);
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }

        void Raise(TerrainDocumentHost &host) {
            TerrainSculptBrush brush;
            brush.amountMeters = 2;
            const std::array stamps{TerrainSculptStamp{4, 4}};
            const auto changed = host.Sculpt(host.View().fence, TerrainEditOperationId::Create(1).Value(), brush, stamps);
            REQUIRE(changed.HasValue());
            REQUIRE(host.View().dirty);
            REQUIRE(host.BorrowSource()->heightsMeters[40] == 2);
        }

        Tests::OwnedTestDirectory directory{"horo-terrain-host"};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations;
        DocumentIdentityRegistry registry;
        Assets::AssetImportMetadata metadata;
        std::filesystem::path root, path, sidecar;
    };
}  // namespace

TEST_CASE("Terrain host saves canonical heights and preserves accepted metadata", "[terrain][host]") {
    TerrainProject project;
    auto host = project.Open();
    const auto metadataBefore = Assets::CaptureAssetSource(project.root, project.sidecar, 256 * 1024);
    REQUIRE(metadataBefore.HasValue());
    project.Raise(*host);
    const auto saved = host->Save(host->View().fence, project.mutations, project.files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == Assets::AssetSourcePublicationStatus::Durable);
    REQUIRE_FALSE(host->View().dirty);
    const auto metadataAfter = Assets::CaptureAssetSource(project.root, project.sidecar, 256 * 1024);
    REQUIRE(metadataAfter.HasValue());
    REQUIRE(metadataAfter.Value().identity == metadataBefore.Value().identity);
    auto reopened = project.Open();
    REQUIRE(reopened->BorrowSource()->heightsMeters[40] == 2);
    REQUIRE(host->History(host->View().fence, false).HasValue());
    REQUIRE(host->View().dirty);
    REQUIRE(host->History(host->View().fence, true).HasValue());
    REQUIRE_FALSE(host->View().dirty);
}

TEST_CASE("Terrain host rejects external source or metadata changes without overwriting them", "[terrain][host]") {
    TerrainProject project;
    auto host = project.Open();
    project.Raise(*host);
    SECTION("source changed") {
        const std::array changed{std::byte{42}};
        REQUIRE(project.files.WriteDurable(project.path, changed).HasValue());
    }
    SECTION("metadata changed") {
        project.metadata.importSettings["settings.terrain-spacing-x"] = "2";
        project.WriteMetadata();
    }
    const auto sourceBefore = Assets::CaptureAssetSource(project.root, project.path, 4 * 1024 * 1024);
    const auto metadataBefore = Assets::CaptureAssetSource(project.root, project.sidecar, 256 * 1024);
    REQUIRE(sourceBefore.HasValue());
    REQUIRE(metadataBefore.HasValue());
    const auto rejected = host->Save(host->View().fence, project.mutations, project.files);
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
    REQUIRE(host->View().conflict);
    REQUIRE(host->View().dirty);
    REQUIRE(Assets::CaptureAssetSource(project.root, project.path, 4 * 1024 * 1024).Value().identity == sourceBefore.Value().identity);
    REQUIRE(Assets::CaptureAssetSource(project.root, project.sidecar, 256 * 1024).Value().identity == metadataBefore.Value().identity);
}

TEST_CASE("Terrain host keeps uncertain native publication dirty and can durably retry", "[terrain][host]") {
    TerrainProject project;
    auto host = project.Open();
    project.Raise(*host);
    UncertainFiles uncertain;
    const auto saved = host->Save(host->View().fence, project.mutations, uncertain);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status != Assets::AssetSourcePublicationStatus::Durable);
    REQUIRE(host->View().dirty);
    REQUIRE_FALSE(host->View().conflict);
    REQUIRE(project.Open()->BorrowSource()->heightsMeters[40] == 2);
    const auto retried = host->Save(host->View().fence, project.mutations, project.files);
    REQUIRE(retried.HasValue());
    REQUIRE(retried.Value().status == Assets::AssetSourcePublicationStatus::Durable);
    REQUIRE_FALSE(host->View().dirty);
}

TEST_CASE("Terrain host rejects stale or foreign-thread commands and revokes closed borrows", "[terrain][host]") {
    TerrainProject project;
    auto host = project.Open();
    const auto oldFence = host->View().fence;
    project.Raise(*host);
    const auto rejected = host->Save(oldFence, project.mutations, project.files);
    REQUIRE(rejected.HasError());
    REQUIRE(rejected.ErrorValue().code.Value() == TerrainEditErrors::StaleRevision.code.Value());
    const auto current = host->View().fence;
    bool refused{}, noBorrow{};
    std::thread foreign([&] {
        noBorrow = host->BorrowSource() == nullptr;
        const auto result = host->History(current, false);
        refused = result.HasError() && result.ErrorValue().code.Value() == SourceDocumentErrors::WrongThread.code.Value();
    });
    foreign.join();
    REQUIRE(refused);
    REQUIRE(noBorrow);
    REQUIRE(host->View().dirty);
    REQUIRE(host->Close().HasValue());
    REQUIRE(host->View().closed);
    REQUIRE(host->BorrowSource() == nullptr);
    REQUIRE(host->History(current, false).HasError());
    REQUIRE(host->Save(current, project.mutations, project.files).HasError());
}
