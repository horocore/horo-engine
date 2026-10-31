#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"
#include "OwnedTestDirectory.h"
#include "terrain/TerrainDocumentHost.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    struct LayeredProject final {
        Tests::OwnedTestDirectory directory{"terrain-layered-host"};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        DocumentIdentityRegistry identities;
        Assets::AssetImportMetadata metadata;
        std::filesystem::path source{directory.Path() / "assets/terrain.horoasset"};
        std::filesystem::path sidecar{source.string() + ".horo"};

        LayeredProject() {
            std::filesystem::create_directories(source.parent_path());
            const std::vector<std::byte> zeros(9 * 9 * 4);
            REQUIRE(files.WriteDurable(source, zeros).HasValue());
            const auto contribution = TerrainRawImporterContribution();
            metadata.assetId = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000081").Value();
            metadata.assetType = Assets::AssetTypeId::Parse("core.terrain").Value();
            metadata.importerContributionId = contribution.contributionId;
            metadata.importerVersion = contribution.version;
            metadata.importerPackageId = contribution.packageId;
            metadata.importerModuleId = contribution.moduleId;
            metadata.importerModuleVersion = contribution.moduleVersion;
            metadata.absoluteSourcePath = directory.Path() / "original.terrainraw";
            metadata.sourceExtension = "terrainraw";
            metadata.sourceHash = Assets::HashAssetImportSource(std::vector<std::uint8_t>(zeros.size()));
            metadata.sourceByteSize = zeros.size();
            metadata.importSettings = {{"settings.terrain-width", "9"},     {"settings.terrain-height", "9"},
                                       {"settings.terrain-spacing-x", "1"}, {"settings.terrain-spacing-z", "1"},
                                       {"settings.terrain-origin-x", "0"},  {"settings.terrain-origin-z", "0"},
                                       {"settings.terrain-crs", ""}};
            metadata.lastImportReasons = {Assets::AssetImportReason::InitialImport};
            metadata.importedAtUtc = "2026-11-01T00:00:00.000Z";
            WriteMetadata();
        }

        void WriteMetadata() {
            const auto bytes = Assets::SerializeAssetImportMetadata(metadata);
            REQUIRE(bytes.HasValue());
            REQUIRE(files.WriteDurable(sidecar, std::as_bytes(std::span(bytes.Value()))).HasValue());
        }

        std::shared_ptr<TerrainDocumentHost> Open() {
            const auto identity = identities.Open({DocumentKind::Asset, SourceDocumentId::Parse("assets/terrain.horoasset").Value()});
            REQUIRE(identity.HasValue());
            auto host = TerrainDocumentHost::Open(directory.Path(), source, metadata.assetId, identity.Value().identity, metadata.assetId,
                                                  mutations);
            REQUIRE(host.HasValue());
            return std::move(host).Value();
        }

        Assets::AssetSourceCapture Capture(const std::filesystem::path &path) const {
            auto captured = Assets::CaptureAssetSource(directory.Path(), path, 1024 * 1024);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        void Convert(TerrainDocumentHost &host) {
            REQUIRE(host.Convert(host.View().fence, std::nullopt, mutations, files).HasValue());
            REQUIRE(host.View().containerSource);
        }
    };
}  // namespace

TEST_CASE("Explicit clean terrain conversion preserves source and revokes every old session fence", "[terrain][layered-host]") {
    LayeredProject project;
    auto host = project.Open();
    const auto old = host->View();
    const auto original = *host->BorrowSource();
    const auto metadata = project.Capture(project.sidecar).identity;
    REQUIRE_FALSE(old.containerSource);
    project.Convert(*host);
    const auto current = host->View();
    CHECK(current.fence.session != old.fence.session);
    CHECK(current.fence.expectedRevision.Value() > old.fence.expectedRevision.Value());
    CHECK(current.fence.dataset == old.fence.dataset);
    CHECK_FALSE(current.dirty);
    CHECK(host->BorrowSource()->heightsMeters == original.heightsMeters);
    CHECK(host->BorrowSource()->holes == std::vector<std::uint8_t>(81, 0));
    CHECK(project.Capture(project.sidecar).identity == metadata);
    CHECK(Terrain::IsTerrainAuthoringSource(project.Capture(project.source).bytes));
    CHECK(host->Save(old.fence, project.mutations, project.files).HasError());
    CHECK(host->History(old.fence, false).HasError());
    CHECK(host->CaptureSavedSource(old.fence).HasError());
    REQUIRE(host->CaptureSavedSource(current.fence).HasValue());
    CHECK(host->BorrowLayerBinding() == nullptr);
}

TEST_CASE("Terrain holes survive real save reopen and exact history without a layer binding", "[terrain][layered-host]") {
    LayeredProject project;
    auto host = project.Open();
    project.Convert(*host);
    TerrainPaintBrush brush;
    brush.mode = TerrainPaintMode::CarveHole;
    brush.radiusMeters = 2;
    const std::array stamps{TerrainSculptStamp{4, 4}};
    const TerrainPaintFence fence{.document = host->View().fence};
    const auto carved = host->Paint(fence, TerrainEditOperationId::Create(1).Value(), brush, stamps, project.mutations);
    REQUIRE(carved.HasValue());
    REQUIRE(host->View().dirty);
    REQUIRE(host->BorrowSource()->holes[40] == 1);
    const auto expected = host->BorrowSource()->holes;
    REQUIRE(host->Save(host->View().fence, project.mutations, project.files).HasValue());
    REQUIRE_FALSE(host->View().dirty);
    auto reopened = project.Open();
    REQUIRE(reopened->View().containerSource);
    CHECK(reopened->BorrowSource()->holes == expected);
    CHECK(reopened->BorrowSource()->heightsMeters == host->BorrowSource()->heightsMeters);
    REQUIRE(host->History(host->View().fence, false).HasValue());
    CHECK(host->BorrowSource()->holes == std::vector<std::uint8_t>(81, 0));
    REQUIRE(host->History(host->View().fence, true).HasValue());
    CHECK(host->BorrowSource()->holes == expected);
    REQUIRE_FALSE(host->View().dirty);
}

TEST_CASE("Dirty legacy terrain cannot be silently converted or lose its unsaved height edits", "[terrain][layered-host]") {
    LayeredProject project;
    auto host = project.Open();
    TerrainSculptBrush brush;
    brush.amountMeters = 2;
    const std::array stamps{TerrainSculptStamp{4, 4}};
    REQUIRE(host->Sculpt(host->View().fence, TerrainEditOperationId::Create(1).Value(), brush, stamps).HasValue());
    const auto dirty = host->View();
    const auto source = project.Capture(project.source).identity;
    REQUIRE(host->Convert(dirty.fence, std::nullopt, project.mutations, project.files).HasError());
    CHECK(host->View().fence.session == dirty.fence.session);
    CHECK(host->View().fence.expectedRevision == dirty.fence.expectedRevision);
    CHECK(host->View().dirty);
    CHECK_FALSE(host->View().containerSource);
    CHECK(host->BorrowSource()->heightsMeters[40] == 2);
    CHECK(project.Capture(project.source).identity == source);
    REQUIRE(host->Save(host->View().fence, project.mutations, project.files).HasValue());
    project.Convert(*host);
    CHECK(host->BorrowSource()->heightsMeters[40] == 2);
    CHECK(project.Open()->BorrowSource()->heightsMeters[40] == 2);
}

TEST_CASE("Terrain conversion never overwrites an externally replaced source or metadata", "[terrain][layered-host]") {
    LayeredProject project;
    auto host = project.Open();
    SECTION("External source replacement") {
        const std::array bytes{std::byte{42}};
        REQUIRE(project.files.WriteDurable(project.source, bytes).HasValue());
    }
    SECTION("External metadata replacement") {
        project.metadata.importSettings["settings.terrain-spacing-x"] = "2";
        project.WriteMetadata();
    }
    const auto source = project.Capture(project.source).identity;
    const auto metadata = project.Capture(project.sidecar).identity;
    const auto before = host->View();
    REQUIRE(host->Convert(before.fence, std::nullopt, project.mutations, project.files).HasError());
    CHECK(project.Capture(project.source).identity == source);
    CHECK(project.Capture(project.sidecar).identity == metadata);
    CHECK(host->View().fence.session == before.fence.session);
    CHECK_FALSE(host->View().containerSource);
}
