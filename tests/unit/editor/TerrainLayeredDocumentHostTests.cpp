#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetReimport.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"
#include "terrain/TerrainDocumentHost.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    /** @brief Models a noncooperating external replacement after admission but before publication. */
    class ReimportReplacementFiles final : public TestSupport::NativePublicationFiles {
    public:
        std::function<void()> beforeStage;
        bool replaced{};

        void ReplaceOnce() {
            if (replaced)
                return;
            replaced = true;
            beforeStage();
        }

        Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override {
            ReplaceOnce();
            return native.WriteDurable(path, bytes);
        }

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            ReplaceOnce();
            return native.WritePrivateDurable(path, bytes, created, permissions);
        }
    };

    struct LayeredProject final {
        Tests::OwnedTestDirectory directory{"terrain-layered-host"};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        DocumentIdentityRegistry identities;
        Assets::AssetRegistry assets;
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

        std::shared_ptr<TerrainDocumentHost> Open(const Assets::AssetRegistrySnapshot &registry = {}) {
            const auto identity = identities.Open({DocumentKind::Asset, SourceDocumentId::Parse("assets/terrain.horoasset").Value()});
            REQUIRE(identity.HasValue());
            auto host = TerrainDocumentHost::Open(directory.Path(), source, metadata.assetId, identity.Value().identity, metadata.assetId,
                                                  mutations, registry);
            REQUIRE(host.HasValue());
            return std::move(host).Value();
        }

        Assets::AssetSourceCapture Capture(const std::filesystem::path &path) const {
            auto captured = Assets::CaptureAssetSource(directory.Path(), path, 1024 * 1024);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        Terrain::TerrainConfigurationSnapshot Configuration() const {
            using namespace Terrain;
            return TerrainConfigurationSnapshot::Create({.configuration = TerrainConfigurationRevision::Create(1).Value(),
                                                         .capability = TerrainCapabilityRevision::Create(1).Value(),
                                                         .tier = TerrainFeatureTier::Ultra,
                                                         .limits = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value().limits})
                .Value();
        }

        std::filesystem::path LayerPath() const {
            return directory.Path() / "assets/layers/basic.horoasset";
        }

        Terrain::TerrainAuthoringLayerSet PublishLayerSet() {
            using namespace Terrain;
            const auto material = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000082").Value();
            const auto layerAsset = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000083").Value();
            TerrainAuthoringLayerSet source{.asset = layerAsset,
                                            .layers = {.id = TerrainLayerSetId::Create(layerAsset.Bytes()).Value(),
                                                       .revision = TerrainContentRevision::Create(1).Value(),
                                                       .layerCount = 2}};
            const auto first = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000084").Value();
            const auto second = Assets::AssetId::Parse("00000000-0000-0000-0000-000000000085").Value();
            const auto materialId = TerrainMaterialAssetId::Create(material.Bytes()).Value();
            source.layers.layers[0] = {TerrainLayerId::Create(first.Bytes()).Value(), materialId, 1000};
            source.layers.layers[1] = {TerrainLayerId::Create(second.Bytes()).Value(), materialId, 1000};
            auto encoded = EncodeTerrainAuthoringLayerSet(source, Configuration());
            REQUIRE(encoded.HasValue());
            // Project-provided material source is opaque to geometry tools: only its real tracked identity is asserted.
            Application::PreparedAuthoredProjectAssetBatch batch{.operationId = "layered-host-fixture",
                                                                 .projectRoot = directory.Path(),
                                                                 .items = {{material,
                                                                            Assets::AssetTypeId::Parse("core.material").Value(),
                                                                            ProjectPath::Parse("assets/material.horoasset").Value(),
                                                                            {std::byte{0x31}}},
                                                                           {layerAsset,
                                                                            Assets::AssetTypeId::Parse("core.terrain.layerset").Value(),
                                                                            ProjectPath::Parse("assets/layers/basic.horoasset").Value(),
                                                                            std::move(encoded).Value()}}};
            Application::ProjectAssetImportCommitter publisher{mutations, files, &assets};
            REQUIRE(publisher.CommitAuthored(batch).HasValue());
            return source;
        }

        void Convert(TerrainDocumentHost &host) {
            REQUIRE(host.Convert(host.View().fence, std::nullopt, mutations, files).HasValue());
            REQUIRE(host.View().containerSource);
        }

        void CheckNoReimportStages() const {
            for (const auto &entry : std::filesystem::directory_iterator(source.parent_path())) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("horo-reimport-") == std::string::npos);
                CHECK(name.find("source-publication.tmp") == std::string::npos);
            }
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

TEST_CASE("Converted terrain rejects generic RAW reimport before historical upstream source access", "[terrain][layered-host]") {
    LayeredProject project;
    auto host = project.Open();
    project.Convert(*host);
    Assets::AssetImporterCatalog catalog;
    REQUIRE(catalog.Register(TerrainRawImporterContribution()).HasValue());
    const auto published = catalog.Publish();
    REQUIRE(published.HasValue());
    Assets::AssetRegistry registry;
    auto candidate = Assets::PrepareAssetRegistryCandidate(project.directory.Path());
    REQUIRE(candidate.HasValue());
    REQUIRE(registry.Publish(std::move(candidate).Value()).status != Assets::AssetRegistryBuildStatus::Failed);
    REQUIRE_FALSE(std::filesystem::exists(project.metadata.absoluteSourcePath));
    const auto source = project.Capture(project.source).identity;
    const auto metadata = project.Capture(project.sidecar).identity;
    const auto revision = registry.Snapshot().Revision();
    const auto rejected = Assets::ReimportProjectAsset({.absoluteProjectRoot = project.directory.Path(),
                                                        .absoluteAssetPath = project.source,
                                                        .importerCatalog = published.Value().get(),
                                                        .registry = &registry,
                                                        .files = &project.files},
                                                       {});
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == Terrain::TerrainSourceErrors::UnsupportedFormat.code.Value());
    CHECK(project.Capture(project.source).identity == source);
    CHECK(project.Capture(project.sidecar).identity == metadata);
    CHECK(registry.Snapshot().Revision() == revision);
    project.CheckNoReimportStages();
}

TEST_CASE("Generic RAW reimport preserves a destination replaced after initial admission", "[terrain][layered-host]") {
    LayeredProject project;
    const auto raw = project.Capture(project.source).bytes;
    auto host = project.Open();
    project.Convert(*host);
    const auto container = project.Capture(project.source).bytes;
    REQUIRE(project.files.WriteDurable(project.source, raw).HasValue());
    auto upstream = raw;
    upstream[2] = std::byte{0x80};  // First little-endian Float32 sample is 1 metre; original payload is all zeroes.
    upstream[3] = std::byte{0x3f};
    REQUIRE(project.files.WriteDurable(project.metadata.absoluteSourcePath, upstream).HasValue());
    Assets::AssetImporterCatalog catalog;
    REQUIRE(catalog.Register(TerrainRawImporterContribution()).HasValue());
    const auto published = catalog.Publish();
    REQUIRE(published.HasValue());
    Assets::AssetRegistry registry;
    auto candidate = Assets::PrepareAssetRegistryCandidate(project.directory.Path());
    REQUIRE(candidate.HasValue());
    REQUIRE(registry.Publish(std::move(candidate).Value()).status != Assets::AssetRegistryBuildStatus::Failed);
    const auto revision = registry.Snapshot().Revision();
    auto expectedSource = project.Capture(project.source).identity;
    auto expectedMetadata = project.Capture(project.sidecar).identity;
    ReimportReplacementFiles external;
    SECTION("Converted container replaces RAW payload during staging") {
        external.beforeStage = [&] {
            REQUIRE(project.files.WriteDurable(project.source, container).HasValue());
            expectedSource = project.Capture(project.source).identity;
        };
    }
    SECTION("Import provenance changes during staging") {
        external.beforeStage = [&] {
            project.metadata.importSettings["settings.terrain-spacing-x"] = "2";
            project.WriteMetadata();
            expectedMetadata = project.Capture(project.sidecar).identity;
        };
    }
    const auto rejected = Assets::ReimportProjectAsset({.absoluteProjectRoot = project.directory.Path(),
                                                        .absoluteAssetPath = project.source,
                                                        .importerCatalog = published.Value().get(),
                                                        .registry = &registry,
                                                        .files = &external},
                                                       {});
    REQUIRE(external.replaced);
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
    CHECK(project.Capture(project.source).identity == expectedSource);
    CHECK(project.Capture(project.sidecar).identity == expectedMetadata);
    CHECK(registry.Snapshot().Revision() == revision);
    project.CheckNoReimportStages();
}

TEST_CASE("Reimport retains the visible new source pair when only derived index persistence fails", "[terrain][layered-host]") {
    LayeredProject project;
    auto upstream = project.Capture(project.source).bytes;
    upstream[2] = std::byte{0x80};
    upstream[3] = std::byte{0x3f};
    REQUIRE(project.files.WriteDurable(project.metadata.absoluteSourcePath, upstream).HasValue());
    Assets::AssetImporterCatalog catalog;
    REQUIRE(catalog.Register(TerrainRawImporterContribution()).HasValue());
    const auto published = catalog.Publish();
    REQUIRE(published.HasValue());
    Assets::AssetRegistry registry;
    auto candidate = Assets::PrepareAssetRegistryCandidate(project.directory.Path());
    REQUIRE(candidate.HasValue());
    REQUIRE(registry.Publish(std::move(candidate).Value()).status != Assets::AssetRegistryBuildStatus::Failed);
    const auto revision = registry.Snapshot().Revision();
    const auto oldMetadata = project.Capture(project.sidecar).identity;
    const auto index = project.directory.Path() / std::filesystem::path{ProjectLayout::AssetIndexPath};
    REQUIRE_FALSE(std::filesystem::exists(index));
    REQUIRE(std::filesystem::create_directories(index));  // Genuine native persistence failure after registry publication.
    const auto result = Assets::ReimportProjectAsset({.absoluteProjectRoot = project.directory.Path(),
                                                      .absoluteAssetPath = project.source,
                                                      .importerCatalog = published.Value().get(),
                                                      .registry = &registry,
                                                      .files = &project.files},
                                                     {});
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == Assets::AssetSourceErrors::DurabilityUnconfirmed.code.Value());
    CHECK(project.Capture(project.source).bytes == upstream);
    CHECK(project.Capture(project.sidecar).identity != oldMetadata);
    const auto metadata = Assets::ReadAssetImportMetadata(project.sidecar);
    REQUIRE(metadata.HasValue());
    const auto bytes = std::span(upstream);
    const std::vector<std::uint8_t> sourceBytes(reinterpret_cast<const std::uint8_t *>(bytes.data()),
                                                reinterpret_cast<const std::uint8_t *>(bytes.data()) + bytes.size());
    CHECK(metadata.Value().sourceHash == Assets::HashAssetImportSource(sourceBytes));
    CHECK(registry.Snapshot().Revision() != revision);
    REQUIRE(registry.Snapshot().Find(project.metadata.assetId) != nullptr);
    project.CheckNoReimportStages();
}

TEST_CASE("Layered terrain retains normalized weights and semantic binding through conversion and reopen", "[terrain][layered-host]") {
    LayeredProject project;
    const auto layerSource = project.PublishLayerSet();
    auto host = project.Open(project.assets.Snapshot());
    REQUIRE(host->BorrowLayerSets().size() == 1);
    const auto binding = host->BorrowLayerSets().front().binding;
    CHECK(binding.asset == layerSource.asset);
    REQUIRE(host->Convert(host->View().fence, binding, project.mutations, project.files).HasValue());
    REQUIRE(host->BorrowSource()->layerCount == 2);
    REQUIRE(host->BorrowSource()->weights[80] == 65535);
    REQUIRE(host->BorrowSource()->weights[81] == 0);
    TerrainPaintBrush brush;
    brush.mode = TerrainPaintMode::Weights;
    brush.strength = 1;
    brush.radiusMeters = 2;
    brush.targets[0] = {binding.order[1], 1};
    brush.targetCount = 1;
    const TerrainPaintFence fence{host->View().fence, binding.id, binding.revision, binding.digest};
    const std::array stamps{TerrainSculptStamp{4, 4}};
    REQUIRE(host->Paint(fence, TerrainEditOperationId::Create(1).Value(), brush, stamps, project.mutations).HasValue());
    REQUIRE(host->BorrowSource()->weights[80] == 0);
    REQUIRE(host->BorrowSource()->weights[81] == 65535);
    const auto weights = host->BorrowSource()->weights;
    REQUIRE(host->Save(host->View().fence, project.mutations, project.files).HasValue());
    // Omitted replacement means preserve authored layer meaning, not silently remove all channels.
    REQUIRE(host->Convert(host->View().fence, std::nullopt, project.mutations, project.files).HasValue());
    REQUIRE(host->BorrowLayerBinding() != nullptr);
    CHECK(host->BorrowLayerBinding()->id == binding.id);
    CHECK(host->BorrowSource()->weights == weights);
    auto reopened = project.Open(project.assets.Snapshot());
    REQUIRE(reopened->BorrowLayerBinding() != nullptr);
    CHECK(reopened->BorrowLayerBinding()->id == binding.id);
    CHECK(reopened->BorrowSource()->weights == weights);
}

TEST_CASE("Layer painting rejects externally replaced semantic source even without registry publication", "[terrain][layered-host]") {
    LayeredProject project;
    auto layerSource = project.PublishLayerSet();
    auto host = project.Open(project.assets.Snapshot());
    REQUIRE(host->BorrowLayerSets().size() == 1);
    const auto binding = host->BorrowLayerSets().front().binding;
    REQUIRE(host->Convert(host->View().fence, binding, project.mutations, project.files).HasValue());
    const auto weights = host->BorrowSource()->weights;
    const auto terrainIdentity = project.Capture(project.source).identity;
    const auto registryRevision = project.assets.Snapshot().Revision();
    layerSource.layers.revision = Terrain::TerrainContentRevision::Create(2).Value();
    const auto replacement = Terrain::EncodeTerrainAuthoringLayerSet(layerSource, project.Configuration());
    REQUIRE(replacement.HasValue());
    REQUIRE(project.files.WriteDurable(project.LayerPath(), replacement.Value()).HasValue());
    TerrainPaintBrush brush;
    brush.mode = TerrainPaintMode::Weights;
    brush.targets[0] = {binding.order[1], 1};
    brush.targetCount = 1;
    const TerrainPaintFence stale{host->View().fence, binding.id, binding.revision, binding.digest};
    const std::array stamps{TerrainSculptStamp{4, 4}};
    REQUIRE(host->Paint(stale, TerrainEditOperationId::Create(1).Value(), brush, stamps, project.mutations).HasError());
    CHECK(host->BorrowSource()->weights == weights);
    CHECK_FALSE(host->View().dirty);
    CHECK(project.Capture(project.source).identity == terrainIdentity);
    CHECK(project.assets.Snapshot().Revision() == registryRevision);
}
