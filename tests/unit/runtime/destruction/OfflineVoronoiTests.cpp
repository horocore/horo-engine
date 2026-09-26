#include "Horo/Destruction/OfflineVoronoi.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace Horo::Destruction {
    namespace {
        OfflineVoronoiSource Cube() {
            OfflineVoronoiSource source;
            std::array<std::uint8_t, 16> assetBytes{};
            assetBytes[15] = 7;
            source.asset = Assets::AssetId::FromBytes(assetBytes);
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
            recipe.seed = 123456789;
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

        OfflineVoronoiSource ConcavePrism() {
            auto source = Cube();
            source.positions.clear();
            source.indices.clear();
            source.materialSlots.clear();
            std::map<std::array<int, 3>, std::uint32_t> vertices;
            const auto vertex = [&](const std::array<int, 3> &point) {
                const auto [iterator, inserted] = vertices.emplace(point, static_cast<std::uint32_t>(source.positions.size()));
                if (inserted)
                    source.positions.push_back({static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])});
                return iterator->second;
            };
            const auto face = [&](const std::array<std::array<int, 3>, 4> &corners) {
                const std::array<std::uint32_t, 4> indices{vertex(corners[0]), vertex(corners[1]), vertex(corners[2]), vertex(corners[3])};
                source.indices.insert(source.indices.end(), {indices[0], indices[1], indices[2], indices[0], indices[2], indices[3]});
                source.materialSlots.insert(source.materialSlots.end(), {9, 9});
            };
            const auto occupied = [](const int x, const int y) {
                return x >= 0 && x < 2 && y >= 0 && y < 2 && !(x == 1 && y == 1);
            };
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    if (!occupied(x, y))
                        continue;
                    face({{{x, y, 0}, {x, y + 1, 0}, {x + 1, y + 1, 0}, {x + 1, y, 0}}});
                    face({{{x, y, 1}, {x + 1, y, 1}, {x + 1, y + 1, 1}, {x, y + 1, 1}}});
                    if (!occupied(x - 1, y))
                        face({{{x, y, 0}, {x, y, 1}, {x, y + 1, 1}, {x, y + 1, 0}}});
                    if (!occupied(x + 1, y))
                        face({{{x + 1, y, 0}, {x + 1, y + 1, 0}, {x + 1, y + 1, 1}, {x + 1, y, 1}}});
                    if (!occupied(x, y - 1))
                        face({{{x, y, 0}, {x + 1, y, 0}, {x + 1, y, 1}, {x, y, 1}}});
                    if (!occupied(x, y + 1))
                        face({{{x, y + 1, 0}, {x, y + 1, 1}, {x + 1, y + 1, 1}, {x + 1, y + 1, 0}}});
                }
            }
            source.digest = ComputeOfflineVoronoiSourceDigest(source);
            return source;
        }

        void CheckError(const Result<OfflineVoronoiCandidate> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        void CheckClosedCollision(const OfflineVoronoiCollisionPiece &piece) {
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<unsigned, int>> edges;
            for (const auto &triangle : piece.triangles) {
                for (const auto [from, to] :
                     {std::pair{triangle[0], triangle[1]}, std::pair{triangle[1], triangle[2]}, std::pair{triangle[2], triangle[0]}}) {
                    auto &edge = edges[std::minmax(from, to)];
                    ++edge.first;
                    edge.second += from < to ? 1 : -1;
                }
            }
            CHECK_FALSE(edges.empty());
            for (const auto &[edge, incidence] : edges) {
                (void)edge;
                CHECK(incidence.first == 2);
                CHECK(incidence.second == 0);
            }
        }
    }  // namespace

    TEST_CASE("Offline Voronoi produces closed clipped chunks, interiors, and connectivity", "[destruction][voronoi]") {
        const auto source = Cube();
        auto recipe = Recipe();
        recipe.siteIds = {DestructionChunkId::Create(9).Value(), DestructionChunkId::Create(7).Value()};
        auto generated = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(generated.HasValue());
        const auto &candidate = generated.Value();
        REQUIRE(candidate.chunks.size() == 2);
        CHECK(candidate.sourceAsset == source.asset);
        CHECK(candidate.sourceRevision == source.revision);
        CHECK(candidate.sourceDigest == source.digest);
        CHECK(candidate.recipeId == recipe.id);
        CHECK(candidate.recipeRevision == recipe.revision);
        CHECK(candidate.toolchainDigest == recipe.toolchainDigest);
        CHECK(candidate.schemaVersion == OfflineVoronoiSchemaVersion);
        CHECK(candidate.chunks[0].id.Value() == 9);
        CHECK(candidate.chunks[1].id.Value() == 7);
        CHECK(candidate.chunks[0].volume == Catch::Approx(0.5));
        CHECK(candidate.chunks[1].volume == Catch::Approx(0.5));
        CHECK(candidate.chunks[0].centerOfMass[0] == Catch::Approx(0.25));
        CHECK(candidate.chunks[1].centerOfMass[0] == Catch::Approx(0.75));
        REQUIRE(candidate.chunks[0].neighbors.size() == 1);
        REQUIRE(candidate.chunks[1].neighbors.size() == 1);
        CHECK(candidate.chunks[0].neighbors[0] == candidate.chunks[1].id);
        CHECK(candidate.chunks[1].neighbors[0] == candidate.chunks[0].id);
        for (const auto &chunk : candidate.chunks) {
            const auto interiors = std::count_if(chunk.triangles.begin(), chunk.triangles.end(), [](const auto &triangle) {
                return triangle.interior && triangle.materialSlot == 17;
            });
            CHECK(interiors >= 2);
            for (const auto &triangle : chunk.triangles)
                CHECK(triangle.indices[2] < chunk.positions.size());
            for (const auto &piece : chunk.collisionPieces)
                CheckClosedCollision(piece);
        }
        CHECK(candidate.estimatedBytes > 0);
        CHECK(candidate.workItems > 0);
    }

    TEST_CASE("Offline Voronoi repeats exact topology and fingerprints under one toolchain", "[destruction][voronoi]") {
        const auto source = Cube();
        auto recipe = Recipe();
        recipe.sites.clear();
        recipe.siteCount = 3;
        recipe.siteIds.push_back(DestructionChunkId::Create(3).Value());
        const auto a = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        const auto b = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(a.HasValue());
        REQUIRE(b.HasValue());
        CHECK(a.Value().semanticFingerprint == b.Value().semanticFingerprint);
        CHECK(a.Value().estimatedBytes == b.Value().estimatedBytes);
        CHECK(a.Value().workItems == b.Value().workItems);
        REQUIRE(a.Value().chunks.size() == b.Value().chunks.size());
        for (std::size_t index = 0; index < a.Value().chunks.size(); ++index) {
            const auto &left = a.Value().chunks[index];
            const auto &right = b.Value().chunks[index];
            CHECK(left.id == right.id);
            CHECK(left.site == right.site);
            CHECK(left.positions == right.positions);
            REQUIRE(left.triangles.size() == right.triangles.size());
            for (std::size_t triangle = 0; triangle < left.triangles.size(); ++triangle) {
                CHECK(left.triangles[triangle].indices == right.triangles[triangle].indices);
                CHECK(left.triangles[triangle].materialSlot == right.triangles[triangle].materialSlot);
                CHECK(left.triangles[triangle].interior == right.triangles[triangle].interior);
            }
            REQUIRE(left.collisionPieces.size() == right.collisionPieces.size());
            for (std::size_t piece = 0; piece < left.collisionPieces.size(); ++piece) {
                CHECK(left.collisionPieces[piece].positions == right.collisionPieces[piece].positions);
                CHECK(left.collisionPieces[piece].triangles == right.collisionPieces[piece].triangles);
                CHECK(left.collisionPieces[piece].volume == right.collisionPieces[piece].volume);
            }
            CHECK(left.neighbors == right.neighbors);
            CHECK(left.volume == right.volume);
            CHECK(left.centerOfMass == right.centerOfMass);
        }
        recipe.seed++;
        const auto changed = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().semanticFingerprint != a.Value().semanticFingerprint);
        recipe.toolchainDigest.bytes[1] = 1;
        const auto changedToolchain = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(changedToolchain.HasValue());
        CHECK(changedToolchain.Value().semanticFingerprint != changed.Value().semanticFingerprint);
    }

    TEST_CASE("Offline Voronoi preserves distinct exterior material slots through clipping", "[destruction][voronoi]") {
        auto source = ConcavePrism();
        for (std::size_t index = 0; index < source.materialSlots.size(); ++index)
            source.materialSlots[index] = static_cast<std::uint32_t>(index + 1U);
        source.digest = ComputeOfflineVoronoiSourceDigest(source);
        auto recipe = Recipe();
        recipe.sites = {{{0.5, 0.5, 0.5}, {1.5, 0.5, 0.5}}};
        const auto result = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(result.HasValue());
        std::set<std::uint32_t> materials;
        for (const auto &chunk : result.Value().chunks) {
            for (const auto &triangle : chunk.triangles) {
                if (!triangle.interior)
                    materials.insert(triangle.materialSlot);
            }
        }
        CHECK(materials == std::set<std::uint32_t>(source.materialSlots.begin(), source.materialSlots.end()));
    }

    TEST_CASE("Offline Voronoi decomposes a validated concave source into convex collision pieces", "[destruction][voronoi]") {
        const auto source = ConcavePrism();
        auto recipe = Recipe();
        recipe.siteCount = 1;
        recipe.siteIds.resize(1);
        recipe.sites = {{{0.5, 0.5, 0.5}}};
        const auto uncut = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(uncut.HasValue());
        REQUIRE(uncut.Value().chunks.size() == 1);
        CHECK(uncut.Value().chunks[0].volume == Catch::Approx(3.0));
        CHECK(uncut.Value().chunks[0].collisionPieces.size() >= 3);
        for (const auto &piece : uncut.Value().chunks[0].collisionPieces)
            CheckClosedCollision(piece);
        for (const auto &triangle : uncut.Value().chunks[0].triangles) {
            const auto &chunk = uncut.Value().chunks[0];
            const auto &a = chunk.positions[triangle.indices[0]];
            const auto &b = chunk.positions[triangle.indices[1]];
            const auto &c = chunk.positions[triangle.indices[2]];
            const bool hiddenSeam = a[1] == 1.0F && b[1] == 1.0F && c[1] == 1.0F && a[0] <= 1.0F && b[0] <= 1.0F && c[0] <= 1.0F;
            CHECK_FALSE(hiddenSeam);
        }
        recipe.siteCount = 2;
        recipe.siteIds.push_back(DestructionChunkId::Create(2).Value());
        recipe.sites = {{{0.5, 0.5, 0.5}, {1.5, 0.5, 0.5}}};
        const auto first = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().chunks.size() == 2);
        CHECK(first.Value().chunks[0].volume == Catch::Approx(2.0));
        CHECK(first.Value().chunks[1].volume == Catch::Approx(1.0));
        CHECK(first.Value().chunks[0].collisionPieces.size() >= 2);
        CHECK(first.Value().chunks[1].collisionPieces.size() >= 1);
        for (const auto &chunk : first.Value().chunks) {
            for (const auto &piece : chunk.collisionPieces)
                CheckClosedCollision(piece);
        }
        CHECK(first.Value().chunks[0].neighbors == std::vector<DestructionChunkId>{first.Value().chunks[1].id});
        CHECK(first.Value().chunks[1].neighbors == std::vector<DestructionChunkId>{first.Value().chunks[0].id});
        const auto second = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(second.HasValue());
        CHECK(first.Value().semanticFingerprint == second.Value().semanticFingerprint);
        for (std::size_t index = 0; index < first.Value().chunks.size(); ++index) {
            CHECK(first.Value().chunks[index].positions == second.Value().chunks[index].positions);
            CHECK(first.Value().chunks[index].volume == second.Value().chunks[index].volume);
        }
        recipe.maximumConvexRegions = 1;
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::LimitExceeded);
    }

    TEST_CASE("Offline Voronoi rejects malformed source, sites, provenance, and bounds", "[destruction][voronoi]") {
        auto source = Cube();
        auto recipe = Recipe();
        source.positions[0][0] = std::numeric_limits<float>::infinity();
        source.digest = ComputeOfflineVoronoiSourceDigest(source);
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidMesh);
        source = Cube();
        source.indices.pop_back();
        source.digest = ComputeOfflineVoronoiSourceDigest(source);
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidMesh);
        source = Cube();
        source.digest.bytes[0] ^= 1;
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidInput);
        source = Cube();
        recipe.sites[1] = recipe.sites[0];
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidSites);
        recipe = Recipe();
        recipe.sites[1][0] = 2.0;
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidSites);
        recipe = Recipe();
        recipe.maximumTriangles = 1;
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::LimitExceeded);
        recipe = Recipe();
        recipe.maximumWorkItems = 1;
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::LimitExceeded);
        recipe = Recipe();
        recipe.siteIds[1] = recipe.siteIds[0];
        CheckError(GenerateOfflineVoronoi(source, recipe, CancellationToken{}), OfflineVoronoiErrors::InvalidInput);
    }

    TEST_CASE("Offline Voronoi cancellation and owner revision fence preserve prior candidate", "[destruction][voronoi]") {
        auto source = Cube();
        auto recipe = Recipe();
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CheckError(GenerateOfflineVoronoi(source, recipe, cancelled.Token()), OfflineVoronoiErrors::Cancelled);
        OfflineVoronoiOwner owner;
        const auto revision = owner.Revision();
        const auto token = owner.Token();
        auto first = GenerateOfflineVoronoi(source, recipe, token);
        REQUIRE(first.HasValue());
        REQUIRE(owner.Accept(std::move(first.Value()), revision, source, recipe).HasValue());
        CHECK(token.IsCancellationRequested());
        const auto published = owner.Snapshot();
        REQUIRE(published != nullptr);
        auto modified = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(modified.HasValue());
        auto tampered = modified.Value();
        REQUIRE_FALSE(tampered.chunks[0].triangles.empty());
        ++tampered.chunks[0].triangles[0].materialSlot;
        auto invalid = owner.Accept(std::move(tampered), owner.Revision(), source, recipe);
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == OfflineVoronoiErrors::InvalidInput.code.Value());
        CHECK(owner.Snapshot() == published);
        auto alteredTotals = modified.Value();
        ++alteredTotals.workItems;
        auto invalidTotals = owner.Accept(std::move(alteredTotals), owner.Revision(), source, recipe);
        REQUIRE(invalidTotals.HasError());
        CHECK(invalidTotals.ErrorValue().code.Value() == OfflineVoronoiErrors::InvalidInput.code.Value());
        CHECK(owner.Snapshot() == published);
        auto stale = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(stale.HasValue());
        source.revision++;
        CHECK(owner.Accept(std::move(stale.Value()), owner.Revision(), source, recipe).HasError());
        CHECK(owner.Snapshot() == published);
        source.revision--;
        auto staleSettings = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(staleSettings.HasValue());
        auto changedRecipe = recipe;
        changedRecipe.seed++;
        CHECK(owner.Accept(std::move(staleSettings.Value()), owner.Revision(), source, changedRecipe).HasError());
        CHECK(owner.Snapshot() == published);
        REQUIRE(owner.Invalidate().HasValue());
        auto replaced = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(replaced.HasValue());
        CHECK(owner.Accept(std::move(replaced.Value()), revision, source, recipe).HasError());
        owner.Shutdown();
        CHECK(owner.Token().IsCancellationRequested());
        CHECK(owner.Invalidate().HasError());
        auto afterShutdown = GenerateOfflineVoronoi(source, recipe, CancellationToken{});
        REQUIRE(afterShutdown.HasValue());
        CHECK(owner.Accept(std::move(afterShutdown.Value()), owner.Revision(), source, recipe).HasError());
        CHECK(owner.Snapshot() == published);
    }
}  // namespace Horo::Destruction
