#include "Horo/Destruction/ChunkMeshCook.h"
#include "Horo/Destruction/StructuralGraphCook.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

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

        struct StructuralGraphFixture final {
            std::shared_ptr<const ChunkMeshArtifact> mesh;
            StructuralGraphCookRequest request;
        };

        StructuralGraphFixture GraphFixture(std::uint32_t chunkCount) {
            auto recipe = Recipe();
            if (chunkCount == 3) {
                recipe.siteCount = 3;
                recipe.siteIds.push_back(DestructionChunkId::Create(3).Value());
                recipe.sites.push_back({0.5, 0.5, 0.75});
            }
            const auto generated = GenerateOfflineVoronoi(Cube(), recipe, {});
            REQUIRE(generated.HasValue());
            const auto materials = Materials();
            const auto cooked = Cook(generated.Value(), materials, recipe.limits);
            REQUIRE(cooked.HasValue());
            REQUIRE(cooked.Value()->chunks.size() == chunkCount);
            StructuralGraphFixture fixture;
            fixture.mesh = cooked.Value();
            fixture.request.content = fixture.mesh->content;
            fixture.request.meshIntegrityDigest = fixture.mesh->integrityDigest;
            fixture.request.ownerRevision = StructuralGraphOwnerRevision::Create(1).Value();
            fixture.request.policyRevision = StructuralPolicyRevision::Create(7).Value();
            fixture.request.limits = recipe.limits;
            for (const auto &chunk : fixture.mesh->chunks)
                fixture.request.chunks.push_back({chunk.id, {}, false, true});
            fixture.request.chunks.front().anchor = true;
            return fixture;
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

    TEST_CASE("Structural graph cooks contact support and anchored islands", "[destruction][graph]") {
        auto fixture = GraphFixture(3);
        auto &request = fixture.request;
        request.requiredFeatures.bits = DestructionFeatureBit<DestructionFeature::CookedSupport>;
        request.contacts = {{0, 1, 2.0}, {0, 2, 3.0}, {1, 2, 4.0}};
        const auto first = CookStructuralGraph(*fixture.mesh, request, {});
        const auto second = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value()->integrityDigest == second.Value()->integrityDigest);
        CHECK(first.Value()->validation.islandCount == 1);
        CHECK(first.Value()->validation.contactCount == 3);
        CHECK(first.Value()->chunks[0].supportWeight == 5.0);
        CHECK(first.Value()->chunks[1].supportWeight == 6.0);
        CHECK(first.Value()->chunks[2].supportWeight == 7.0);
        const std::vector<std::uint32_t> expectedAdjacency{1, 2};
        CHECK(first.Value()->chunks[0].adjacency == expectedAdjacency);
        CHECK(first.Value()->chunks[2].flags.initiallySupported);
        request.contacts = {{0, 1, 2.0}};
        request.chunks[2].required = false;
        const auto island = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(island.HasValue());
        CHECK(island.Value()->validation.islandCount == 2);
        CHECK_FALSE(island.Value()->chunks[2].flags.initiallySupported);
        CHECK(island.Value()->chunks[2].island == 1);
        request.chunks[2].required = true;
        const auto disconnected = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(disconnected.HasError());
        CHECK(disconnected.ErrorValue().code.Value() == StructuralGraphErrors::DisconnectedRequired.code.Value());
    }

    TEST_CASE("Structural graph diagnoses invalid contacts and unstable ordering", "[destruction][graph]") {
        auto fixture = GraphFixture(3);
        auto &request = fixture.request;
        request.contacts = {{0, 1, 2.0}, {0, 3, 1.0}};
        const auto invalidIndex = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(invalidIndex.HasError());
        CHECK(invalidIndex.ErrorValue().code.Value() == StructuralGraphErrors::InvalidIndex.code.Value());
        request.contacts = {{0, 1, 2.0}, {0, 1, 2.0}};
        const auto duplicate = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(duplicate.HasError());
        CHECK(duplicate.ErrorValue().code.Value() == StructuralGraphErrors::UnstableOrder.code.Value());
        request.contacts = {{0, 2, 1.0}, {0, 1, 2.0}};
        const auto unstable = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(unstable.HasError());
        CHECK(unstable.ErrorValue().code.Value() == StructuralGraphErrors::UnstableOrder.code.Value());
        std::swap(request.chunks[0], request.chunks[1]);
        const auto unstableChunks = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(unstableChunks.HasError());
        CHECK(unstableChunks.ErrorValue().code.Value() == StructuralGraphErrors::UnstableOrder.code.Value());
        std::swap(request.chunks[0], request.chunks[1]);
        request.contacts = {{0, 1, std::numeric_limits<double>::infinity()}};
        const auto nonfinite = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(nonfinite.HasError());
        CHECK(nonfinite.ErrorValue().code.Value() == StructuralGraphErrors::InvalidInput.code.Value());
    }

    TEST_CASE("Structural graph diagnoses hierarchy cycles", "[destruction][graph]") {
        auto fixture = GraphFixture(3);
        fixture.request.contacts = {{0, 1, 2.0}, {0, 2, 3.0}};
        fixture.request.chunks[0].parent = fixture.request.chunks[1].id;
        fixture.request.chunks[1].parent = fixture.request.chunks[0].id;
        const auto cycle = CookStructuralGraph(*fixture.mesh, fixture.request, {});
        REQUIRE(cycle.HasError());
        CHECK(cycle.ErrorValue().code.Value() == StructuralGraphErrors::HierarchyCycle.code.Value());
    }

    TEST_CASE("Structural graph rejects stale, unsupported, over-budget and cancelled work", "[destruction][graph]") {
        auto fixture = GraphFixture(2);
        auto &request = fixture.request;
        request.contacts = {{0, 1, 1.0}};
        const auto cooked = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value()->validation.workItems > 0);
        auto staleRequest = request;
        ++staleRequest.meshIntegrityDigest.bytes[0];
        const auto stale = CookStructuralGraph(*fixture.mesh, staleRequest, {});
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == StructuralGraphErrors::Stale.code.Value());
        auto unsupportedRequest = request;
        unsupportedRequest.requiredFeatures.bits = DestructionFeatureBit<DestructionFeature::RuntimeGeometryGeneration>;
        const auto unsupported = CookStructuralGraph(*fixture.mesh, unsupportedRequest, {});
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == StructuralGraphErrors::Unsupported.code.Value());
        request.limits.maximumWorkItemsPerTransition = 2;
        const auto bounded = CookStructuralGraph(*fixture.mesh, request, {});
        REQUIRE(bounded.HasError());
        CHECK(bounded.ErrorValue().code.Value() == StructuralGraphErrors::LimitExceeded.code.Value());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto cancelled = CookStructuralGraph(*fixture.mesh, request, cancellation.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == StructuralGraphErrors::Cancelled.code.Value());
    }

    TEST_CASE("Structural graph owner fences stale generations and shutdown", "[destruction][graph]") {
        auto fixture = GraphFixture(2);
        fixture.request.contacts = {{0, 1, 1.0}};
        StructuralGraphCookOwner owner;
        fixture.request.ownerRevision = owner.Revision();
        const auto cooked = CookStructuralGraph(*fixture.mesh, fixture.request, owner.Token());
        REQUIRE(cooked.HasValue());
        const auto revision = owner.Revision();
        const auto token = owner.Token();
        REQUIRE(owner
                    .Accept(cooked.Value(), revision, fixture.request.content, fixture.request.meshIntegrityDigest,
                            fixture.request.policyRevision)
                    .HasValue());
        const auto published = owner.Snapshot();
        CHECK(owner
                  .Accept(cooked.Value(), revision, fixture.request.content, fixture.request.meshIntegrityDigest,
                          StructuralPolicyRevision::Create(8).Value())
                  .HasError());
        CHECK(owner.Snapshot() == published);
        REQUIRE(owner.Invalidate().HasValue());
        CHECK(token.IsCancellationRequested());
        CHECK(owner
                  .Accept(cooked.Value(), revision, fixture.request.content, fixture.request.meshIntegrityDigest,
                          fixture.request.policyRevision)
                  .HasError());
        CHECK(owner
                  .Accept(cooked.Value(), owner.Revision(), fixture.request.content, fixture.request.meshIntegrityDigest,
                          fixture.request.policyRevision)
                  .HasError());
        CHECK(owner.Snapshot() == published);
        fixture.request.ownerRevision = owner.Revision();
        const auto replacement = CookStructuralGraph(*fixture.mesh, fixture.request, owner.Token());
        REQUIRE(replacement.HasValue());
        REQUIRE(owner
                    .Accept(replacement.Value(), owner.Revision(), fixture.request.content, fixture.request.meshIntegrityDigest,
                            fixture.request.policyRevision)
                    .HasValue());
        CHECK(owner.Snapshot() == replacement.Value());
        CHECK(owner.Snapshot() != published);
        owner.Shutdown();
        CHECK(owner.Token().IsCancellationRequested());
        CHECK(owner.Invalidate().HasError());
        CHECK(owner.Snapshot() == replacement.Value());
    }
}  // namespace Horo::Destruction
