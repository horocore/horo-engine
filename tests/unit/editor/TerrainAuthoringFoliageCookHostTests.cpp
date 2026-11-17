#include "../runtime/terrain/FoliageAuthoringTestSupport.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "TerrainAuthoringCookTestSupport.h"

#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::TerrainCookTestSupport {
    namespace {
        constexpr std::string_view TypePath = "assets/authored.foliagetype";

        struct FoliageProject final {
            CookProject project;
            Terrain::FoliageTypeSource type{Terrain::FoliageAuthoringTest::Type()};
            std::vector<std::byte> typeBytes;
            std::vector<Terrain::TerrainAuthoredPlacement> records;
            std::optional<Editor::TerrainAuthoringDocument> document;

            FoliageProject() {
                auto encoded =
                    Terrain::EncodeFoliageTypeSource(type, project.profile.configuration, Terrain::FoliageAuthoringTest::Capabilities());
                REQUIRE(encoded.HasValue());
                typeBytes = std::move(encoded).Value();
                PublishDependencies();
                OpenDocument();
                SeedPlacements();
                SetHole(true, 2);
                Save();
            }

            void PublishDependencies() {
                const auto mesh = Assets::AssetId::FromBytes(type.definition.assets.meshLods[0].Bytes());
                const auto material = Assets::AssetId::FromBytes(type.definition.assets.material.Bytes());
                // These are real tracked project-provided sources, not compiled mesh/material readiness evidence.
                PreparedAuthoredProjectAssetBatch batch{.operationId = "authored-foliage-dependencies",
                                                        .projectRoot = project.directory.Path(),
                                                        .items = {{mesh,
                                                                   Assets::AssetTypeId::Parse("core.mesh").Value(),
                                                                   ProjectPath::Parse("assets/authored-mesh.horoasset").Value(),
                                                                   {std::byte{0x31}}},
                                                                  {material,
                                                                   Assets::AssetTypeId::Parse("core.material").Value(),
                                                                   ProjectPath::Parse("assets/authored-material.horoasset").Value(),
                                                                   {std::byte{0x32}}},
                                                                  {type.asset, Assets::AssetTypeId::Parse("core.foliage.type").Value(),
                                                                   ProjectPath::Parse(TypePath).Value(), typeBytes}}};
                ProjectAssetImportCommitter publisher{project.mutations, project.files, &project.registry};
                auto published = publisher.CommitAuthored(batch);
                REQUIRE(published.HasValue());
                REQUIRE(publisher.LastReceipt().registryPublished);
            }

            void OpenDocument() {
                auto source = project.source.source;
                std::ranges::fill(source.holes, 0);
                auto opened = Editor::TerrainAuthoringDocument::Open(Editor::TerrainDocumentSessionId::Create(1).Value(), std::move(source),
                                                                     project.profile.artifacts.tiles.interiorQuads,
                                                                     Editor::TerrainAuthoringCapability::Edit);
                REQUIRE(opened.HasValue());
                document.emplace(std::move(opened).Value());
            }

            void SeedPlacements() {
                records = Terrain::FoliageAuthoringTest::Source().placements;
                records.push_back({Terrain::FoliageAuthoringTest::Id<Terrain::FoliageInstanceId>(9), type.definition.type, 4, 4, 0, 1, 0});
                Editor::TerrainEditOperation operation{document->Fence(), Editor::TerrainEditOperationId::Create(1).Value(), {}, {}};
                for (const auto &record : records)
                    operation.placements.push_back({record.id, record});
                const auto accepted = document->Execute(operation);
                REQUIRE(accepted.HasValue());
                REQUIRE(accepted.Value().changed);
                REQUIRE(document->Placements().size() == 3);
            }

            void SetHole(bool hole, std::uint64_t operationId) {
                Editor::TerrainRasterPatch patch{.rect = {4, 4, 1, 1}, .holes = {static_cast<std::uint8_t>(hole)}};
                const auto accepted = document->Execute(
                    {document->Fence(), Editor::TerrainEditOperationId::Create(operationId).Value(), {std::move(patch)}, {}});
                REQUIRE(accepted.HasValue());
                REQUIRE(accepted.Value().changed);
            }

            /** @brief Publish saved bytes through the actual project and native source lease authorities. */
            void PublishBytes(const std::filesystem::path &path, std::span<const std::byte> bytes) {
                auto authority = project.mutations.TryAcquire(
                    {project.directory.Path(), Editor::ProjectMutationOwner::Save, "terrain.foliage.fixture.save"});
                REQUIRE(authority.HasValue());
                auto lockPath = Assets::AssetSourcePublicationLockPath(project.directory.Path(), path);
                REQUIRE(lockPath.HasValue());
                auto lease = project.files.TryAcquireExclusive(lockPath.Value(), "terrain.foliage.fixture.source");
                REQUIRE(lease.HasValue());
                auto accepted = Assets::CaptureAssetSource(project.directory.Path(), path, 1024 * 1024);
                REQUIRE(accepted.HasValue());
                auto published = Assets::PublishAssetSource(project.directory.Path(), path, accepted.Value().identity, bytes, lease.Value(),
                                                            project.files);
                REQUIRE(published.HasValue());
                REQUIRE(published.Value().status == Assets::AssetSourcePublicationStatus::Durable);
            }

            void Save() {
                project.source.source = document->Source();
                project.source.foliage.placements.clear();
                for (const auto &[id, value] : document->Placements())
                    project.source.foliage.placements.push_back(value);
                project.source.foliage.definitions = {
                    {type.asset, type.definition.type, type.definition.revision, ComputeSha256(typeBytes)}};
                auto encoded = Terrain::EncodeTerrainAuthoringSource(project.source);
                REQUIRE(encoded.HasValue());
                PublishBytes(project.SourcePath(), encoded.Value());
                REQUIRE(document->AcceptSavedState(document->Fence().session, document->State()).HasValue());
                REQUIRE_FALSE(document->IsDirty());
            }

            /** @brief Independent flat-source grid and known closed-hole cells, without the Application grid adapter. */
            Terrain::CookedFoliagePlacement Expected(bool restored) const {
                std::vector<Terrain::FoliagePlacementSample> samples(81);
                for (auto &sample : samples) {
                    sample.altitudeMillimeters = 2000;
                    sample.density = 65535;
                }
                if (!restored)
                    for (const auto index : {30, 31, 39, 40})
                        samples[index].hole = true;
                auto visible = records;
                if (!restored)
                    visible.pop_back();
                const auto &source = project.source.source;
                Terrain::FoliagePlacementGrid grid{.tile = {source.dataset, {0, 0, 0}},
                                                   .sourceRevision = source.revision,
                                                   .capability = source.capability,
                                                   .spacingXMillimeters = 1000,
                                                   .spacingZMillimeters = 1000,
                                                   .width = 9,
                                                   .height = 9,
                                                   .samples = samples};
                const auto capabilities = Terrain::FoliageAuthoringTest::Capabilities();
                auto definition = Terrain::FoliageTypeDefinition::Create(type.definition, project.profile.configuration, capabilities);
                REQUIRE(definition.HasValue());
                const auto &tiles = project.profile.artifacts.tiles;
                // Authored policy v1 assigns one verified tile per cluster; this independent 9x9 grid has eight cells.
                Terrain::FoliagePlacementCookRequest target{grid,
                                                            definition.Value(),
                                                            capabilities,
                                                            {.version = 1, .cellsPerCluster = 8},
                                                            project.profile.foliage,
                                                            Terrain::TerrainContentRevision::Create(source.revision.Value()).Value(),
                                                            tiles.targetDigest,
                                                            tiles.toolchainDigest,
                                                            tiles.tier};
                auto expected = Terrain::CookAuthoredFoliagePlacement({target, visible, 0, 0});
                REQUIRE(expected.HasValue());
                return std::move(expected).Value();
            }
        };

        /** @brief Bounds every field before interpreting the canonical bundle's fixed little-endian rows. */
        struct Reader final {
            std::span<const std::uint8_t> bytes;
            std::size_t at{72};

            std::span<const std::uint8_t> Take(std::size_t count) {
                REQUIRE(at <= bytes.size());
                REQUIRE(count <= bytes.size() - at);
                const auto result = bytes.subspan(at, count);
                at += count;
                return result;
            }

            std::uint64_t U(unsigned width = 8) {
                const auto field = Take(width);
                std::uint64_t value{};
                for (unsigned i = 0; i < width; ++i)
                    value |= std::uint64_t{field[i]} << (8 * i);
                return value;
            }

            std::span<const std::uint8_t> Payload() {
                const auto count = U();
                REQUIRE(count <= bytes.size() - at);
                return Take(static_cast<std::size_t>(count));
            }
        };

        void CheckInstance(Reader &reader, const Terrain::CookedFoliageInstance &expected) {
            CHECK(std::ranges::equal(reader.Take(16), expected.id.Bytes()));
            CHECK(std::ranges::equal(reader.Take(16), expected.cluster.Bytes()));
            CHECK(std::ranges::equal(reader.Take(16), expected.type.Bytes()));
            CHECK(std::bit_cast<std::int64_t>(reader.U()) == expected.xMillimeters);
            CHECK(std::bit_cast<std::int64_t>(reader.U()) == expected.altitudeMillimeters);
            CHECK(std::bit_cast<std::int64_t>(reader.U()) == expected.zMillimeters);
            CHECK(reader.U(4) == expected.scalePermille);
            CHECK(reader.U(4) == expected.yawMilliDegrees);
            CHECK(reader.U(4) == expected.slopeMilliDegrees);
            CHECK(reader.U(4) == expected.candidateOrdinal);
            CHECK(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(reader.U(2))) == expected.normalXPermille);
            CHECK(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(reader.U(2))) == expected.normalYPermille);
            CHECK(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(reader.U(2))) == expected.normalZPermille);
        }

        void CheckProof(Reader &reader, const Terrain::CookedFoliagePlacement &expected) {
            CHECK(std::ranges::equal(reader.Take(25), Terrain::SerializeTerrainTileId(expected.Tile())));
            CHECK(std::ranges::equal(reader.Take(16), expected.Type().Bytes()));
            CHECK(reader.U() == expected.DefinitionRevision().Value());
            CHECK(reader.U() == expected.ContentRevision().Value());
            CHECK(reader.U() == expected.SourceRevision().Value());
            CHECK(reader.U() == expected.CapabilityRevision().Value());
            CHECK(std::ranges::equal(reader.Take(32), expected.ResultDigest().bytes));
            CHECK(std::ranges::equal(reader.Take(32), expected.Fingerprint().bytes));
            REQUIRE(reader.U() == expected.Instances().size());
            for (const auto &instance : expected.Instances())
                CheckInstance(reader, instance);
        }

        void CheckFoliageBundle(FoliageProject &fixture, const Assets::AssetCookReport &report, bool restored) {
            const auto contents = Assets::ReadCookGenerationContents(fixture.project.files, report.generation, 4 * 1024 * 1024,
                                                                     fixture.project.profile.assets);
            REQUIRE(contents.HasValue());
            REQUIRE(contents.Value().artifacts.size() == 1);
            const auto artifact = Assets::DecodeCookedArtifact(contents.Value().artifacts.front(), fixture.project.profile.assets);
            REQUIRE(artifact.HasValue());
            const auto &payload = artifact.Value().payload;
            REQUIRE(payload.size() >= 104);
            REQUIRE(std::string_view(reinterpret_cast<const char *>(payload.data()), 8) == std::string_view("HTAB\1\0\0\0", 8));
            const auto body = std::span(payload).first(payload.size() - 32);
            CHECK(std::ranges::equal(ComputeSha256(std::as_bytes(body)).bytes, std::span(payload).last(32)));
            Reader reader{body};
            static_cast<void>(reader.Payload());  // Manifest; the shared geometry tests verify its complete semantics.
            REQUIRE(reader.U() == 1);
            static_cast<void>(reader.Payload());
            REQUIRE(reader.U() == 3);  // Required visual, collision and navigation artifacts remain present.
            for (int role = 0; role < 3; ++role)
                CHECK_FALSE(reader.Payload().empty());
            REQUIRE(reader.U() == 1);  // Authored foliage proof is required, never an optional skipped assertion.
            const auto expected = fixture.Expected(restored);
            CheckProof(reader, expected);
            CHECK(reader.at == body.size());
        }
    }  // namespace

    TEST_CASE("Terrain cook consumes saved authored foliage and rebakes restored identities deterministically",
              "[terrain][cook-host][authored-foliage]") {
        FoliageProject fixture;
        const auto first = fixture.project.Cook();
        CHECK(first.cookedAssets == 1);
        CheckFoliageBundle(fixture, first, false);
        const auto visible = fixture.Expected(false);
        REQUIRE(visible.Instances().size() == 2);
        CHECK(visible.Instances()[0].id == fixture.records[0].id);
        CHECK(visible.Instances()[0].altitudeMillimeters == 2250);
        CHECK(visible.Instances()[0].yawMilliDegrees == 28648);
        CHECK(visible.Instances()[1].id == fixture.records[1].id);
        REQUIRE(fixture.document->Placements().size() == 3);
        const auto cached = fixture.project.Cook();
        CHECK(cached.cookedAssets == 0);
        CHECK(cached.cacheHits == 1);
        CheckFoliageBundle(fixture, cached, false);
        auto reordered = fixture.project.source;
        std::ranges::reverse(reordered.foliage.placements);
        const auto encoded = Terrain::EncodeTerrainAuthoringSource(fixture.project.source);
        const auto reversed = Terrain::EncodeTerrainAuthoringSource(reordered);
        REQUIRE(encoded.HasValue());
        REQUIRE(reversed.HasValue());
        CHECK(encoded.Value() == reversed.Value());
        fixture.SetHole(false, 3);
        fixture.Save();
        const auto rebaked = fixture.project.Cook();
        CHECK(rebaked.cookedAssets == 1);
        CheckFoliageBundle(fixture, rebaked, true);
        const auto restored = fixture.Expected(true);
        REQUIRE(restored.Instances().size() == 3);
        CHECK(restored.Instances()[0].id == visible.Instances()[0].id);
        CHECK(restored.Instances()[1].id == visible.Instances()[1].id);
        CHECK(restored.Instances()[2].id == fixture.records[2].id);
        CHECK(restored.Instances()[0].altitudeMillimeters == 2250);
        CHECK(fixture.project.source.foliage.placements.size() == 3);
    }

    TEST_CASE("Terrain cook rejects changed foliage definition bytes and preserves the selected generation",
              "[terrain][cook-host][authored-foliage]") {
        FoliageProject fixture;
        const auto prior = fixture.project.Cook();
        const auto accepted = fixture.project.Request();
        const auto sourceBefore = Assets::CaptureAssetSource(fixture.project.directory.Path(), fixture.project.SourcePath(), 1024 * 1024);
        REQUIRE(sourceBefore.HasValue());
        const auto registryBefore = fixture.project.registry.Snapshot().Revision();
        fixture.type.definition.revision = Terrain::FoliageDefinitionRevision::Create(2).Value();
        auto changed = Terrain::EncodeFoliageTypeSource(fixture.type, fixture.project.profile.configuration,
                                                        Terrain::FoliageAuthoringTest::Capabilities());
        REQUIRE(changed.HasValue());
        fixture.PublishBytes(fixture.project.directory.Path() / TypePath, changed.Value());
        REQUIRE(fixture.project.host->Start(accepted).HasValue());
        auto completed = fixture.project.Completion();
        REQUIRE(completed.result.HasError());
        CHECK(completed.result.ErrorValue().code.Value() == TerrainAuthoringCookErrors::Stale.code.Value());
        const auto current =
            Assets::ResolveCurrentCookGeneration(fixture.project.files, accepted.cookedRoot, fixture.project.profile.assets);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == prior.generation.manifestDigest);
        CHECK(current.Value().generationRoot == prior.generation.generationRoot);
        const auto sourceAfter = Assets::CaptureAssetSource(fixture.project.directory.Path(), fixture.project.SourcePath(), 1024 * 1024);
        REQUIRE(sourceAfter.HasValue());
        CHECK(sourceAfter.Value().identity == sourceBefore.Value().identity);
        CHECK(fixture.project.registry.Snapshot().Revision() == registryBefore);
    }
}  // namespace Horo::Application::TerrainCookTestSupport
