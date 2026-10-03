#include "Horo/Assets/AssetArchive.h"
#include "Horo/Destruction/ChunkCollisionCook.h"
#include "Horo/Physics/PhysicsCookedShapeCache.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <type_traits>

namespace Horo::Destruction {
    static_assert(!std::is_default_constructible_v<ChunkCollisionArtifactSet>);

    namespace {
        /** @brief Constructs closed normalized source geometry independently from cook/publication setup. */
        Assets::PreFracturedSource CollisionSource(std::uint32_t chunkCount) {
            Assets::PreFracturedSource source;
            source.sourceName = "normalized tetrahedron";
            Assets::PreFracturedSourceNode node;
            node.name = "HoroChunk_731__stone";
            node.sourcePath = "geometry/tetrahedron";
            node.geometryToWorld = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            node.positions = {{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}, {0, 0, 4}}};
            node.triangleIndices = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
            node.triangleMaterials.assign(4, "stone");
            if (chunkCount == 2) {
                auto second = node;
                second.name = "HoroChunk_812__stone";
                second.sourcePath = "geometry/second";
                for (auto &position : second.positions)
                    position[0] += 5;
                source.nodes.push_back(std::move(second));
            }
            source.nodes.push_back(std::move(node));
            return source;
        }

        struct CollisionFixture final {
            std::shared_ptr<const ChunkMeshArtifact> mesh;
            std::vector<ChunkCollisionMaterial> bindings;
            ChunkCollisionCookRequest request;

            explicit CollisionFixture(std::uint32_t chunkCount = 1) {
                const auto asset = Assets::AssetId::Parse("64d6b9ce-7e7b-4f68-9dce-5a650c166478").Value();
                const auto limits = GetDestructionTierProfile(DestructionFeatureTier::High).Value().limits;
                const auto source = CollisionSource(chunkCount);
                auto normalized = ValidatePreFracturedSource(source, limits, {});
                REQUIRE(normalized.HasValue());
                Sha256Digest materialDigest;
                materialDigest.bytes[0] = 77;
                const std::array<ImportedChunkMaterialBinding, 1> materials{{{"stone", {4, asset, materialDigest}, false}}};
                const auto content =
                    FractureArtifactContentIdentity::Create(FractureAssetId::Create(asset).Value(),
                                                            FractureContentRevision::Create(6).Value(),
                                                            ComputePreFracturedMeshSemanticDigest(normalized.Value(), asset, 9,
                                                                                                  DestructionFeatureTier::High, materials,
                                                                                                  {}))
                        .Value();
                auto cooked = CookPreFracturedChunkMeshes(normalized.Value(),
                                                          {asset,
                                                           9,
                                                           ComputePreFracturedMeshSourceDigest(normalized.Value()),
                                                           content,
                                                           DestructionFeatureTier::High,
                                                           {},
                                                           limits},
                                                          materials, {});
                REQUIRE(cooked.HasValue());
                mesh = std::move(cooked).Value();
                for (const auto &chunk : mesh->chunks)
                    bindings.push_back({chunk.id, Physics::PhysicsMaterialSlotId::FromValue(45)});
                request.content = mesh->content;
                request.meshIntegrityDigest = mesh->integrityDigest;
                request.target.digest.bytes[0] = 93;
                request.limits = limits;
                request.materials = bindings;
            }
        };
    }  // namespace

    TEST_CASE("DFR projects sealed imported geometry and loads package bytes after source release", "[destruction][collision]") {
        CollisionFixture fixture;
        const auto first = CookChunkCollision(*fixture.mesh, fixture.request);
        REQUIRE(first.HasValue());
        const auto second = CookChunkCollision(*fixture.mesh, fixture.request);
        REQUIRE(second.HasValue());
        REQUIRE(first.Value()->Shapes().size() == 1);
        const auto &artifact = first.Value()->Shapes()[0];
        CHECK(artifact.chunk.Value() == 731);
        CHECK(artifact.shape.payload == second.Value()->Shapes()[0].shape.payload);
        CHECK(first.Value()->Tier() == DestructionFeatureTier::High);
        fixture.mesh.reset();
        auto cache = Physics::PhysicsCookedShapeCache::Create(fixture.request.target).Value();
        auto loaded = cache.Acquire(artifact.shape.descriptor, artifact.shape.payload);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().Compound() != nullptr);
        REQUIRE(loaded.Value().Compound()->children.size() == 1);
        const auto &child = loaded.Value().Compound()->children[0];
        CHECK(child.id.IsValid());
        CHECK(child.material.Value() == 45);
        CHECK(child.hull.vertices.size() == 4);
    }

    TEST_CASE("Multi-chunk collision bundle travels through the production release archive provider", "[destruction][collision][archive]") {
        CollisionFixture fixture{2};
        const auto collision = CookChunkCollision(*fixture.mesh, fixture.request);
        REQUIRE(collision.HasValue());
        const auto encoded = EncodeChunkCollisionArtifacts(*collision.Value());
        REQUIRE(encoded.HasValue());
        CHECK(EncodeChunkCollisionArtifacts(*collision.Value(), encoded.Value().size() - 1).HasError());
        const auto &set = *collision.Value();
        const ChunkCollisionBundleReference reference{set.Content(), set.MeshDigest(), set.Target(),
                                                      ComputeSha256(std::as_bytes(std::span{encoded.Value()}))};
        const auto packageTarget = AssetCookTargetId::Parse("headless-null").Value();
        Assets::AssetCookArtifact envelope;
        envelope.id = set.Content().Asset().Asset();
        const auto type = Assets::AssetTypeId::Parse("core.fracture_collision");
        REQUIRE(type.HasValue());
        envelope.type = type.Value();
        envelope.target = packageTarget;
        envelope.sourceDigest = set.MeshDigest();
        envelope.cacheKeyDigest = set.ConfigurationDigest();
        envelope.payloadDigest = reference.payloadDigest;
        envelope.payload = encoded.Value();
        const auto wrapped = Assets::EncodeCookedArtifact(envelope);
        REQUIRE(wrapped.HasValue());
        const std::array chunks{Assets::AssetChunkDefinition{.id = Assets::AssetChunkId::Parse("core").Value(), .assets = {envelope.id}}};
        const auto plan = Assets::AssetChunkPlan::Create(chunks);
        REQUIRE(plan.HasValue());
        const std::array inputs{Assets::AssetArchiveInput{envelope.id, wrapped.Value()}};
        const auto archive = Assets::BuildAssetArchive(plan.Value(), packageTarget, inputs);
        REQUIRE(archive.HasValue());
        fixture.mesh.reset();
        auto provider = Assets::AssetArchiveProvider::Open(archive.Value(), packageTarget);
        REQUIRE(provider.HasValue());
        auto bytes = provider.Value().Load(envelope.id, {});
        REQUIRE(bytes.HasValue());
        const auto decoded = Assets::DecodeCookedArtifact(bytes.Value());
        REQUIRE(decoded.HasValue());
        auto loaded = LoadChunkCollisionArtifacts(reference, decoded.Value().payload, fixture.request.limits);
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value()->Shapes().size() == 2);
        CHECK(loaded.Value()->Shapes()[0].chunk.Value() == 731);
        CHECK(loaded.Value()->Shapes()[1].chunk.Value() == 812);
        auto cache = Physics::PhysicsCookedShapeCache::Create(reference.target).Value();
        for (const auto &artifact : loaded.Value()->Shapes()) {
            auto shape = cache.Acquire(artifact.shape.descriptor, artifact.shape.payload);
            REQUIRE(shape.HasValue());
            REQUIRE(shape.Value().Compound() != nullptr);
            CHECK(shape.Value().Compound()->children[0].material.Value() == 45);
        }
        CHECK(EncodeChunkCollisionArtifacts(*loaded.Value()).Value() == encoded.Value());
    }

    TEST_CASE("Packaged collision bundles fail atomically for hostile metadata and stale references", "[destruction][collision][archive]") {
        CollisionFixture fixture;
        const auto collision = CookChunkCollision(*fixture.mesh, fixture.request);
        REQUIRE(collision.HasValue());
        auto encoded = EncodeChunkCollisionArtifacts(*collision.Value()).Value();
        auto reference = ChunkCollisionBundleReference{fixture.request.content, fixture.request.meshIntegrityDigest, fixture.request.target,
                                                       ComputeSha256(std::as_bytes(std::span{encoded}))};
        SECTION("schema") {
            encoded[4] = 2;
        }
        SECTION("target") {
            std::as_writable_bytes(std::span{encoded})[96] ^= std::byte{1};
        }
        SECTION("unknown tier") {
            encoded[128] = 255;
        }
        SECTION("too many chunks") {
            encoded[161] = 255;
        }
        SECTION("missing chunk identity") {
            std::fill_n(encoded.begin() + 165, 8, 0);
        }
        SECTION("invalid shape extent") {
            std::fill_n(encoded.begin() + 237, 8, 255);
        }
        SECTION("mixed configuration") {
            std::as_writable_bytes(std::span{encoded})[129] ^= std::byte{1};
        }
        SECTION("corrupt shape") {
            std::as_writable_bytes(std::span{encoded}).back() ^= std::byte{1};
        }
        SECTION("truncation") {
            encoded.pop_back();
        }
        SECTION("trailing bytes") {
            encoded.push_back(0);
        }
        SECTION("stale mesh reference") {
            std::as_writable_bytes(std::span{reference.meshDigest.bytes})[0] ^= std::byte{1};
        }
        SECTION("lowered work limit") {
            fixture.request.limits.maximumWorkItemsPerTransition = 1;
        }
        reference.payloadDigest = ComputeSha256(std::as_bytes(std::span{encoded}));
        CHECK(LoadChunkCollisionArtifacts(reference, encoded, fixture.request.limits).HasError());
    }

    TEST_CASE("Collision bundle decoding rejects every truncated prefix with a valid payload digest", "[destruction][collision][archive]") {
        CollisionFixture fixture{2};
        const auto collision = CookChunkCollision(*fixture.mesh, fixture.request);
        REQUIRE(collision.HasValue());
        const auto encoded = EncodeChunkCollisionArtifacts(*collision.Value());
        REQUIRE(encoded.HasValue());
        ChunkCollisionBundleReference reference{fixture.request.content, fixture.request.meshIntegrityDigest, fixture.request.target, {}};
        const std::span<const std::uint8_t> bytes{encoded.Value()};
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            CAPTURE(size);
            const auto prefix = bytes.first(size);
            reference.payloadDigest = ComputeSha256(std::as_bytes(prefix));
            CHECK(LoadChunkCollisionArtifacts(reference, prefix, fixture.request.limits).HasError());
        }
        reference.payloadDigest = ComputeSha256(std::as_bytes(bytes));
        CHECK(LoadChunkCollisionArtifacts(reference, bytes, fixture.request.limits).HasValue());
    }

    TEST_CASE("DFR collision rejects stale provenance, explicit missing materials and bounded limits", "[destruction][collision]") {
        CollisionFixture fixture;
        SECTION("content revision") {
            fixture.request.content =
                FractureArtifactContentIdentity::Create(fixture.request.content.Asset(), FractureContentRevision::Create(7).Value(),
                                                        fixture.request.content.SemanticDigest())
                    .Value();
        }
        SECTION("mesh revision") {
            std::as_writable_bytes(std::span{fixture.request.meshIntegrityDigest.bytes})[0] ^= std::byte{1};
        }
        SECTION("material missing") {
            fixture.request.materials = {};
        }
        SECTION("wrong chunk material") {
            fixture.bindings[0].chunk = DestructionChunkId::Create(44).Value();
        }
        SECTION("invalid collision slot") {
            fixture.bindings[0].slot = {};
        }
        SECTION("work budget") {
            fixture.request.limits.maximumWorkItemsPerTransition = 1;
        }
        SECTION("output budget") {
            fixture.request.limits.maximumArtifactBytes = 1;
        }
        SECTION("source vertex limit") {
            fixture.request.convex.limits.maxSourceVertices = 3;
        }
        SECTION("hull vertex limit") {
            fixture.request.convex.limits.maxHullVertices = 3;
        }
        SECTION("compound child limit") {
            fixture.request.compound.maximumChildren = 0;
        }
        SECTION("unsupported Physics algorithm") {
            fixture.request.convex.algorithmVersion = 99;
        }
        SECTION("broken source seal") {
            auto corrupt = std::const_pointer_cast<ChunkMeshArtifact>(fixture.mesh);
            corrupt->chunks[0].collisionPieces[0].triangles[0][0] = 999;
        }
        CHECK(CookChunkCollision(*fixture.mesh, fixture.request).HasError());
    }

    TEST_CASE("DFR collision owner cancels stale work and retains last complete generation", "[destruction][collision][lifecycle]") {
        CollisionFixture fixture;
        ChunkCollisionCookOwner owner;
        const auto revision = owner.Revision();
        auto artifact = CookChunkCollision(*fixture.mesh, fixture.request, owner.Token());
        REQUIRE(artifact.HasValue());
        REQUIRE(owner.Accept(artifact.Value(), revision, fixture.request).HasValue());
        auto retained = owner.Snapshot();
        auto oldToken = owner.Token();
        REQUIRE(owner.Invalidate().HasValue());
        CHECK(oldToken.IsCancellationRequested());
        CHECK(CookChunkCollision(*fixture.mesh, fixture.request, oldToken).HasError());
        CHECK(owner.Accept(artifact.Value(), revision, fixture.request).HasError());
        CHECK(owner.Snapshot() == retained);
        auto changed = fixture.request;
        changed.target.digest.bytes[0]++;
        CHECK(owner.Accept(artifact.Value(), owner.Revision(), changed).HasError());
        changed = fixture.request;
        changed.convex.algorithmVersion++;
        CHECK(owner.Accept(artifact.Value(), owner.Revision(), changed).HasError());
        changed = fixture.request;
        const std::array repeatedBindings{fixture.bindings[0], fixture.bindings[0]};
        changed.materials = repeatedBindings;
        CHECK(owner.Accept(artifact.Value(), owner.Revision(), changed).HasError());
        CHECK(owner.Snapshot() == retained);
        fixture.bindings[0].slot = Physics::PhysicsMaterialSlotId::FromValue(46);
        CHECK(owner.Accept(artifact.Value(), owner.Revision(), fixture.request).HasError());
        auto replacement = CookChunkCollision(*fixture.mesh, fixture.request, owner.Token());
        REQUIRE(replacement.HasValue());
        REQUIRE(owner.Accept(replacement.Value(), owner.Revision(), fixture.request).HasValue());
        CHECK(owner.Snapshot() != retained);
        CHECK(retained->Shapes()[0].shape.payload != owner.Snapshot()->Shapes()[0].shape.payload);
        auto pending = owner.Token();
        owner.Shutdown();
        owner.Shutdown();
        CHECK(pending.IsCancellationRequested());
        CHECK(owner.Invalidate().HasError());
        CHECK(owner.Accept(replacement.Value(), owner.Revision(), fixture.request).HasError());
        CHECK(owner.Snapshot() == replacement.Value());
    }
}  // namespace Horo::Destruction
