#include "Horo/Destruction/ChunkMeshCook.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <type_traits>

namespace Horo::Destruction {
    static_assert(!std::is_default_constructible_v<ChunkMeshArtifact>);

    namespace {
        OfflineVoronoiSource Cube() {
            OfflineVoronoiSource source;
            std::array<std::uint8_t, 16> id{};
            id[15] = 7;
            source.asset = Assets::AssetId::FromBytes(id);
            source.revision = 3;
            source.positions = {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}};
            source.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
            source.materialSlots.assign(12, 9);
            source.digest = ComputeOfflineVoronoiSourceDigest(source);
            return source;
        }

        OfflineVoronoiRecipe Recipe() {
            OfflineVoronoiRecipe recipe;
            recipe.id = 11;
            recipe.revision = 2;
            recipe.siteCount = 2;
            recipe.siteIds = {DestructionChunkId::Create(1).Value(), DestructionChunkId::Create(2).Value()};
            recipe.sites = {{{0.25, 0.5, 0.5}, {0.75, 0.5, 0.5}}};
            recipe.interiorMaterialSlot = 17;
            recipe.toolchainVersion = 1;
            recipe.toolchainDigest.bytes[0] = 42;
            recipe.tier = DestructionFeatureTier::High;
            recipe.limits = GetDestructionTierProfile(recipe.tier).Value().limits;
            recipe.maximumVertices = 4096;
            recipe.maximumTriangles = 2048;
            recipe.maximumWorkItems = 100000;
            recipe.maximumConvexRegions = 128;
            return recipe;
        }

        std::array<ChunkMaterialBinding, 2> Materials() {
            std::array<std::uint8_t, 16> exterior{};
            std::array<std::uint8_t, 16> interior{};
            exterior[15] = 8;
            interior[15] = 9;
            std::array<ChunkMaterialBinding, 2> bindings{
                {{9, Assets::AssetId::FromBytes(exterior), {}}, {17, Assets::AssetId::FromBytes(interior), {}}}};
            bindings[0].revisionDigest.bytes[0] = 1;
            bindings[1].revisionDigest.bytes[0] = 2;
            return bindings;
        }

        FractureArtifactContentIdentity Content(const OfflineVoronoiCandidate &source, std::span<const ChunkMaterialBinding> materials,
                                                ChunkUvPolicy uv = {}) {
            const auto asset = FractureAssetId::Create(source.sourceAsset).Value();
            const auto revision = FractureContentRevision::Create(5).Value();
            return FractureArtifactContentIdentity::Create(asset, revision, ComputeChunkMeshSemanticDigest(source, materials, uv)).Value();
        }

        auto Cook(const OfflineVoronoiCandidate &candidate, std::span<const ChunkMaterialBinding> materials,
                  const DestructionLimits &limits, const CancellationToken &token = {}) {
            return CookChunkMeshes(candidate, Content(candidate, materials), materials, {}, limits, token);
        }

        Assets::PreFracturedSource ImportedHalves() {
            const auto cube = Cube();
            Assets::PreFracturedSource source;
            source.sourceName = "split.fbx";
            for (std::uint32_t half = 0; half < 2; ++half) {
                Assets::PreFracturedSourceNode node;
                node.name = "HoroChunk_" + std::to_string(half + 1);
                node.sourcePath = "nodes/" + std::to_string(half);
                node.geometryToWorld = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
                node.positions = cube.positions;
                for (auto &position : node.positions)
                    position[0] = position[0] * 0.5F + half * 0.5F;
                node.triangleIndices = cube.indices;
                for (std::size_t triangle = 0; triangle < cube.indices.size(); triangle += 3) {
                    const bool cut = node.positions[cube.indices[triangle]][0] == 0.5F &&
                                     node.positions[cube.indices[triangle + 1]][0] == 0.5F &&
                                     node.positions[cube.indices[triangle + 2]][0] == 0.5F;
                    node.triangleMaterials.push_back(cut ? "Cut" : "Stone");
                }
                source.nodes.push_back(std::move(node));
            }
            return source;
        }

        std::array<ImportedChunkMaterialBinding, 2> ImportedMaterials() {
            const auto materials = Materials();
            return {{{"Cut", materials[1], true}, {"Stone", materials[0], false}}};
        }

        FractureArtifactContentIdentity ImportedContent(const PreFracturedCandidate &source,
                                                        std::span<const ImportedChunkMaterialBinding> materials) {
            const auto asset = FractureAssetId::Create(Cube().asset).Value();
            return FractureArtifactContentIdentity::Create(asset, FractureContentRevision::Create(6).Value(),
                                                           ComputePreFracturedMeshSemanticDigest(source, Cube().asset, 3,
                                                                                                 DestructionFeatureTier::High, materials,
                                                                                                 {}))
                .Value();
        }
    }  // namespace

    TEST_CASE("Chunk mesh cook preserves faces, basis, material and mass provenance", "[destruction][mesh]") {
        const auto source = Cube();
        const auto recipe = Recipe();
        const auto generated = GenerateOfflineVoronoi(source, recipe, {});
        REQUIRE(generated.HasValue());
        const auto materials = Materials();
        const auto first = Cook(generated.Value(), materials, recipe.limits);
        const auto second = Cook(generated.Value(), materials, recipe.limits);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        const auto &artifact = *first.Value();
        CHECK(artifact.chunks.size() == 2);
        CHECK(artifact.integrityDigest == second.Value()->integrityDigest);
        CHECK(artifact.sourceAsset == source.asset);
        CHECK(artifact.sourceRevision == source.revision);
        CHECK(artifact.sourceDigest == source.digest);
        CHECK(artifact.recipeId == recipe.id);
        CHECK(artifact.materials[1].asset == materials[1].asset);
        CHECK(artifact.tier == DestructionFeatureTier::High);
        CHECK(artifact.producedFeatures.Contains(DestructionFeature::PreCookedFracture));
        std::size_t interiors{};
        double volume{};
        for (const auto &chunk : artifact.chunks) {
            volume += chunk.mass.volume;
            CHECK(chunk.mass.firstMoment[0] / chunk.mass.volume >= 0.0);
            for (const auto &face : chunk.faces) {
                if (face.interior) {
                    ++interiors;
                    CHECK(face.materialSlot == recipe.interiorMaterialSlot);
                }
                const auto &vertex = chunk.vertices[face.indices[0]];
                CHECK(vertex.tangent[3] == 1.0F);
                CHECK(std::isfinite(vertex.uv[0]));
                CHECK(std::isfinite(vertex.uv[1]));
            }
            CHECK(chunk.mass.minimum[0] <= chunk.mass.maximum[0]);
        }
        CHECK(interiors > 0);
        CHECK(volume == Catch::Approx(1.0));
    }

    TEST_CASE("Chunk mesh cook rejects missing materials, damaged cuts, stale inputs and budgets", "[destruction][mesh]") {
        const auto source = Cube();
        const auto recipe = Recipe();
        auto generated = GenerateOfflineVoronoi(source, recipe, {});
        REQUIRE(generated.HasValue());
        const auto materials = Materials();
        const std::array<ChunkMaterialBinding, 1> exterior{materials[0]};
        const auto missing = Cook(generated.Value(), exterior, recipe.limits);
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == ChunkMeshCookErrors::MissingMaterial.code.Value());
        auto changed = generated.Value();
        changed.chunks[0].triangles[0].interior = !changed.chunks[0].triangles[0].interior;
        const auto damaged = Cook(changed, materials, recipe.limits);
        REQUIRE(damaged.HasError());
        CHECK(damaged.ErrorValue().code.Value() == ChunkMeshCookErrors::InvalidInput.code.Value());
        auto lowLimits = recipe.limits;
        lowLimits.maximumArtifactBytes = 100;
        const auto limited = Cook(generated.Value(), materials, lowLimits);
        REQUIRE(limited.HasError());
        CHECK(limited.ErrorValue().code.Value() == ChunkMeshCookErrors::LimitExceeded.code.Value());
        auto staleContent = Content(generated.Value(), materials);
        auto changedMaterials = materials;
        changedMaterials[1].revisionDigest.bytes[0] = 3;
        const auto stale = CookChunkMeshes(generated.Value(), staleContent, changedMaterials, {}, recipe.limits, {});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == ChunkMeshCookErrors::Stale.code.Value());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto cancelled = Cook(generated.Value(), materials, recipe.limits, cancellation.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == ChunkMeshCookErrors::Cancelled.code.Value());
    }

    TEST_CASE("Chunk mesh owner replaces atomically and fences stale work and shutdown", "[destruction][mesh]") {
        const auto source = Cube();
        const auto recipe = Recipe();
        const auto generated = GenerateOfflineVoronoi(source, recipe, {});
        REQUIRE(generated.HasValue());
        const auto materials = Materials();
        const auto artifact = Cook(generated.Value(), materials, recipe.limits);
        REQUIRE(artifact.HasValue());
        ChunkMeshCookOwner owner;
        const auto revision = owner.Revision();
        const auto token = owner.Token();
        REQUIRE(owner.Accept(artifact.Value(), revision, artifact.Value()->content).HasValue());
        const auto published = owner.Snapshot();
        REQUIRE(published == artifact.Value());
        REQUIRE(owner.Invalidate().HasValue());
        CHECK(token.IsCancellationRequested());
        CHECK(owner.Accept(artifact.Value(), revision, artifact.Value()->content).HasError());
        CHECK(owner.Snapshot() == published);
        owner.Shutdown();
        CHECK(owner.Token().IsCancellationRequested());
        CHECK(owner.Accept(artifact.Value(), owner.Revision(), artifact.Value()->content).HasError());
        CHECK(owner.Invalidate().HasError());
        CHECK(owner.Snapshot() == published);
    }

    TEST_CASE("Imported chunks cook paired cut faces, exact materials and mass", "[destruction][mesh]") {
        auto prepared = ValidatePreFracturedSource(ImportedHalves(), Recipe().limits, {});
        REQUIRE(prepared.HasValue());
        const auto materials = ImportedMaterials();
        const auto digest = ComputePreFracturedMeshSourceDigest(prepared.Value());
        const ImportedChunkMeshCookRequest request{Cube().asset,
                                                   3,
                                                   digest,
                                                   ImportedContent(prepared.Value(), materials),
                                                   DestructionFeatureTier::High,
                                                   {},
                                                   Recipe().limits};
        const auto cooked = CookPreFracturedChunkMeshes(prepared.Value(), request, materials, {});
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value()->sourceDigest == digest);
        CHECK(cooked.Value()->chunks.size() == 2);
        CHECK(cooked.Value()->tier == DestructionFeatureTier::High);
        std::size_t interiors{};
        double volume{};
        for (const auto &chunk : cooked.Value()->chunks) {
            volume += chunk.mass.volume;
            for (const auto &face : chunk.faces)
                interiors += face.interior ? 1 : 0;
            CHECK(chunk.mass.firstMoment[0] / chunk.mass.volume == Catch::Approx(chunk.id.Value() == 1 ? 0.25 : 0.75));
        }
        CHECK(interiors == 4);
        CHECK(volume == Catch::Approx(1.0));
    }

    TEST_CASE("Imported chunk cook rejects unmatched or misclassified cuts and stale provenance", "[destruction][mesh]") {
        auto prepared = ValidatePreFracturedSource(ImportedHalves(), Recipe().limits, {});
        REQUIRE(prepared.HasValue());
        auto materials = ImportedMaterials();
        const auto digest = ComputePreFracturedMeshSourceDigest(prepared.Value());
        auto content = ImportedContent(prepared.Value(), materials);
        ImportedChunkMeshCookRequest request{Cube().asset,
                                             3,
                                             digest,
                                             ImportedContent(prepared.Value(), std::span{materials}.subspan(1)),
                                             DestructionFeatureTier::High,
                                             {},
                                             Recipe().limits};
        auto missing = CookPreFracturedChunkMeshes(prepared.Value(), request, std::span{materials}.subspan(1), {});
        REQUIRE(missing.HasError());
        CHECK(missing.ErrorValue().code.Value() == ChunkMeshCookErrors::MissingMaterial.code.Value());
        materials[0].interior = false;
        content = ImportedContent(prepared.Value(), materials);
        request.content = content;
        auto invalid = CookPreFracturedChunkMeshes(prepared.Value(), request, materials, {});
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == ChunkMeshCookErrors::InvalidInterior.code.Value());
        materials = ImportedMaterials();
        content = ImportedContent(prepared.Value(), materials);
        auto staleDigest = digest;
        ++staleDigest.bytes[0];
        request.content = content;
        request.sourceDigest = staleDigest;
        auto stale = CookPreFracturedChunkMeshes(prepared.Value(), request, materials, {});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == ChunkMeshCookErrors::Stale.code.Value());
    }
}  // namespace Horo::Destruction
