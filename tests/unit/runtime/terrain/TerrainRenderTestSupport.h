#pragma once
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/TerrainRenderIntegration/TerrainTileResourceOwner.h"
#include "support/TerrainMaterialFixtures.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests::TerrainRendering {
    using namespace Terrain;
    using namespace TerrainRender;
    using namespace TerrainMaterials;

    inline Sha256Digest Digest(const std::uint8_t byte) {
        Sha256Digest d{};
        d.bytes[0] = byte;
        return d;
    }

    inline TerrainCanonicalSource Source(const bool holes = false, const std::uint32_t width = 5) {
        TerrainCanonicalSource s;
        s.dataset = Stable<TerrainDatasetId>(1);
        SerializedTerrainIdentity asset{};
        asset[0] = 9;
        s.sourceAsset = Assets::AssetId::FromBytes(asset);
        s.revision = IdentityValue<TerrainSourceRevision>(1);
        s.capability = IdentityValue<TerrainCapabilityRevision>(1);
        s.width = s.height = width;
        s.coordinates.originX = 10'000'000'000.0;
        s.coordinates.originZ = -10'000'000'000.0;
        s.coordinates.spacingX = s.coordinates.spacingZ = 2;
        s.heightsMeters.assign(width * width, 5);
        s.layerCount = 1;
        s.weights.assign(width * width, 65535);
        s.holes.assign(width * width, holes ? 1 : 0);
        return s;
    }

    inline CookedTerrainSourceArtifacts Cook(const TerrainCanonicalSource &s) {
        TerrainSourceArtifactProfile p;
        p.tiles.interiorQuads = 4;
        p.tiles.targetDigest = Digest(7);
        p.tiles.toolchainDigest = Digest(8);
        auto cooked = CookTerrainSourceArtifacts(s, p, {}, {});
        REQUIRE(cooked.HasValue());
        return std::move(cooked).Value();
    }

    inline TerrainPayloadManifest Manifest(const CookedTerrainSourceArtifacts &c) {
        TerrainPayloadManifestRequest r;
        r.terrain = &c;
        r.content = IdentityValue<TerrainContentRevision>(1);
        r.terrainRequirements.visual = TerrainPayloadRequirement::Required;
        auto m = GenerateTerrainPayloadManifest(r);
        REQUIRE(m.HasValue());
        return std::move(m).Value();
    }

    struct SourceFixture {
        TerrainCanonicalSource source;
        CookedTerrainSourceArtifacts cooked;
        TerrainPayloadManifest manifest;
        TerrainConfigurationSnapshot configuration{Configuration()};
        std::vector<TerrainRenderTileSelection> selections;

        explicit SourceFixture(const bool holes = false, const std::uint32_t width = 5)
            : source(Source(holes, width)), cooked(Cook(source)), manifest(Manifest(cooked)) {
            for (const auto &a : cooked.Artifacts())
                if (a.role == TerrainSourceArtifactRole::Visual)
                    selections.push_back({a.tile, a.digest, LayerData(1)});
        }

        TerrainRenderSnapshotRequest Request(const std::uint64_t generation = 1) const {
            TerrainRenderSnapshotRequest r{.source = cooked, .manifest = manifest, .configuration = configuration};
            r.lifecycle = TerrainRuntimeLifecycle::Active;
            r.tiles = selections;
            auto &h = r.header;
            h.terrain = {source.dataset, {0, 1}};
            h.revisions = {IdentityValue<TerrainContentRevision>(1), IdentityValue<TerrainResidencyRevision>(1),
                           IdentityValue<TerrainMutationRevision>(1), IdentityValue<TerrainCapabilityRevision>(1)};
            h.world = Stable<WorldStreaming::WorldPartitionId>(11);
            h.origin = {IdentityValue<WorldStreaming::OriginFrameId>(1), IdentityValue<WorldStreaming::OriginFrameRevision>(1),
                        IdentityValue<WorldStreaming::OriginGeneration>(1)};
            h.generation = generation;
            h.configuration = configuration.Data().configuration;
            h.target = source.dataset.IsValid() ? Digest(7) : Sha256Digest{};
            h.manifest = manifest.Digest();
            constexpr std::array caps{TerrainFoliageCapability::TerrainRuntime, TerrainFoliageCapability::RenderExtraction};
            h.capabilities = TerrainFoliageCapabilitySet::Create(caps).Value();
            return r;
        }

        TerrainRenderExtractionSnapshot Capture(const std::uint64_t generation = 1) const {
            auto r = CaptureTerrainRenderSnapshot(Request(generation));
            REQUIRE(r.HasValue());
            return std::move(r).Value();
        }
    };

    struct ResourceFixture {
        std::unique_ptr<Render::RenderFrontend> frontend;
        std::unique_ptr<TerrainTileResourceOwner> owner;

        explicit ResourceFixture(const Render::RenderResourceUploadLimits limits = {}, const std::uint32_t maximumRetiring = 4) {
            Render::RenderBackendRegistry registry;
            REQUIRE(Render::RegisterNullRenderBackend(registry).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto created = Render::RenderFrontend::Create(registry, Render::RenderBackendId{"null"}, {}, limits);
            REQUIRE(created.HasValue());
            frontend = std::move(created).Value();
            owner = std::make_unique<TerrainTileResourceOwner>(*frontend, maximumRetiring);
        }

        ~ResourceFixture() {
            (void)owner->BeginShutdown();
            (void)owner->DrainRetired();
        }

        void Publish(SourceFixture &source, const std::uint64_t generation = 1, const std::uint64_t expected = 0) {
            REQUIRE(owner->Stage(source.Capture(generation), source.Request(generation).header, expected).HasValue());
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            auto progress = owner->Advance(source.Request(generation).header);
            REQUIRE(progress.HasValue());
            if (!progress.Value()) {
                REQUIRE(frontend->ProcessResourceRequests().HasValue());
                progress = owner->Advance(source.Request(generation).header);
            }
            REQUIRE(progress.HasValue());
            REQUIRE(progress.Value());
        }
    };
}  // namespace Horo::Tests::TerrainRendering
