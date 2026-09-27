#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCGTerrain/TerrainInputSnapshot.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>
#include <type_traits>

namespace Horo::PCGTerrain {
    namespace {
        template <typename Id> Id Number(const std::uint64_t value) {
            return Id::Create(value).Value();
        }

        Terrain::TerrainDatasetId Dataset() {
            Terrain::SerializedTerrainIdentity bytes{};
            bytes[0] = 7;
            return Terrain::TerrainDatasetId::Create(bytes).Value();
        }

        Terrain::TerrainFoliageCapabilitySet QueryGrant() {
            const Terrain::TerrainFoliageCapability query[] = {Terrain::TerrainFoliageCapability::TerrainQuery};
            return Terrain::TerrainFoliageCapabilitySet::Create(query).Value();
        }

        Terrain::TerrainDatasetRegistration Registration(const std::uint64_t content = 3, const std::int64_t originOffsetMillimeters = 0) {
            const auto profile = Terrain::GetTerrainTierProfile(Terrain::TerrainFeatureTier::Baseline).Value();
            auto configuration =
                Terrain::TerrainConfigurationSnapshot::Create({.configuration = Number<Terrain::TerrainConfigurationRevision>(1),
                                                               .capability = Number<Terrain::TerrainCapabilityRevision>(1),
                                                               .tier = Terrain::TerrainFeatureTier::Baseline,
                                                               .limits = profile.limits});
            REQUIRE(configuration.HasValue());
            Terrain::TerrainDatasetDescriptorData data{};
            data.dataset = Dataset();
            data.content = Number<Terrain::TerrainContentRevision>(content);
            data.bounds = {.revision = Number<Terrain::TerrainBoundsRevision>(1),
                           .minimum = Math::WorldCoordinate64::FromMillimeters(originOffsetMillimeters, -1'000, originOffsetMillimeters),
                           .maximum = Math::WorldCoordinate64::FromMillimeters(originOffsetMillimeters + 10'000, 2'000,
                                                                               originOffsetMillimeters + 10'000)};
            data.grid = {.samplesX = 129, .samplesZ = 129, .tileInteriorQuads = 128, .lodLevels = 1, .layersPerTile = 4};
            data.footprint = {.activeTerrainTiles = 1,
                              .activeFoliageClusters = 1,
                              .activeFoliageInstances = 1,
                              .residentTerrainBytes = 1'000'000,
                              .residentFoliageBytes = 1'000,
                              .stagingBytes = 1'000,
                              .retiringBytes = 1'000,
                              .workItems = 1'000};
            auto descriptor = Terrain::TerrainDatasetDescriptor::Create(data, configuration.Value());
            REQUIRE(descriptor.HasValue());
            return {descriptor.Value(), {Terrain::TerrainFeatureTierBit<Terrain::TerrainFeatureTier::Baseline>}, QueryGrant()};
        }

        Terrain::TerrainFoliageRegistry Registry(const bool grant = true) {
            return Terrain::TerrainFoliageRegistry::Create(Number<Terrain::TerrainFoliageRegistryInstanceId>(1),
                                                           grant ? QueryGrant() : Terrain::TerrainFoliageCapabilitySet::Empty())
                .Value();
        }

        TerrainInputCandidate Candidate(const std::uint64_t snapshot = 1, const std::uint64_t spatialRevision = 1,
                                        const std::uint64_t content = 3) {
            TerrainInputCandidate candidate{};
            candidate.snapshot = Number<PCG::SpatialSnapshotId>(snapshot);
            candidate.provenance = {Number<PCG::SpatialProviderId>(2), Number<PCG::SpatialSourceId>(3),
                                    Number<PCG::SpatialRevision>(spatialRevision)};
            candidate.dataset = Dataset();
            candidate.revision = {Number<Terrain::TerrainContentRevision>(content), Number<Terrain::TerrainResidencyRevision>(1),
                                  Number<Terrain::TerrainMutationRevision>(1), Number<Terrain::TerrainCapabilityRevision>(1)};
            candidate.boundsRevision = Number<Terrain::TerrainBoundsRevision>(1);
            candidate.bounds = {{0.0F, -1.0F, 0.0F}, {10.0F, 2.0F, 10.0F}};
            candidate.coverage = PCG::PCGSpatialCoverage::Complete;
            candidate.samplesX = 2;
            candidate.samplesZ = 2;
            candidate.samples
                .assign(4, {.height = 0.5F, .normal = {0.0F, 1.0F, 0.0F}, .slopeRadians = 0.0F, .materialLayer = 2, .exclusionMask = 255});
            return candidate;
        }

        PCG::PCGSpatialCurrentness Current(const std::uint64_t spatialRevision = 1, const std::uint64_t originEpoch = 1) {
            return {Number<PCG::SpatialProviderId>(2), Number<PCG::SpatialSourceId>(3), Number<PCG::SpatialRevision>(spatialRevision),
                    originEpoch};
        }

        template <typename Value> void ErrorIs(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Terrain input copies height slope material and mask without mutable Terrain access", "[unit][pcg][terrain]") {
        auto registry = Registry();
        REQUIRE(registry.RegisterDataset(Registration()).HasValue());
        auto candidate = Candidate();
        candidate.samples[0].exclusionMask = 0;
        auto captured = CaptureTerrainInput(registry, candidate);
        REQUIRE(captured.HasValue());
        candidate.samples[0].height = 2.0F;
        candidate.samples[0].materialLayer = 0;
        const auto snapshot = captured.Value();
        static_assert(std::is_same_v<decltype(snapshot.Samples()), std::span<const TerrainInputSample>>);
        CHECK(snapshot.Samples()[0].height == 0.5F);
        CHECK(snapshot.Samples()[0].materialLayer == 2);
        CHECK(snapshot.Samples()[0].exclusionMask == 0);
        CHECK(snapshot.Samples()[1].exclusionMask == 255);
        CHECK(snapshot.SamplesX() == 2);
        CHECK(snapshot.SamplesZ() == 2);
        CHECK(snapshot.Dataset() == Dataset());
        const auto corner = snapshot.SampleAt(1, 1);
        REQUIRE(corner.HasValue());
        CHECK(corner.Value().position == Math::Vec3{10.0F, 0.5F, 10.0F});
        CHECK(corner.Value().sample.materialLayer == 2);
        ErrorIs(snapshot.SampleAt(2, 0), PCG::PCGErrors::SpatialInputInvalid);
        REQUIRE(ValidateTerrainInputCurrent(registry, snapshot, snapshot.Revision(), Current()).HasValue());
    }

    TEST_CASE("Terrain input rejects incomplete coverage malformed samples and capacity", "[unit][pcg][terrain]") {
        auto registry = Registry();
        REQUIRE(registry.RegisterDataset(Registration()).HasValue());
        auto partial = Candidate();
        partial.coverage = PCG::PCGSpatialCoverage::Partial;
        ErrorIs(CaptureTerrainInput(registry, partial), PCG::PCGErrors::SpatialCoverageUnavailable);
        auto missing = Candidate();
        missing.samples.pop_back();
        ErrorIs(CaptureTerrainInput(registry, missing), PCG::PCGErrors::SpatialInputInvalid);
        auto invalidNormal = Candidate();
        invalidNormal.samples[0].normal = {0.0F, -1.0F, 0.0F};
        ErrorIs(CaptureTerrainInput(registry, invalidNormal), PCG::PCGErrors::SpatialInputInvalid);
        auto invalidSlope = Candidate();
        invalidSlope.samples[0].slopeRadians = 1.0F;
        ErrorIs(CaptureTerrainInput(registry, invalidSlope), PCG::PCGErrors::SpatialInputInvalid);
        auto invalidHeight = Candidate();
        invalidHeight.samples[0].height = std::numeric_limits<float>::quiet_NaN();
        ErrorIs(CaptureTerrainInput(registry, invalidHeight), PCG::PCGErrors::SpatialInputInvalid);
        auto invalidMaterial = Candidate();
        invalidMaterial.samples[0].materialLayer = 4;
        ErrorIs(CaptureTerrainInput(registry, invalidMaterial), PCG::PCGErrors::SpatialInputInvalid);
        auto outside = Candidate();
        outside.bounds.maximum.x = 11.0F;
        ErrorIs(CaptureTerrainInput(registry, outside), PCG::PCGErrors::SpatialInputInvalid);
        auto oversized = Candidate();
        oversized.samplesX = 130;
        oversized.samplesZ = 130;
        ErrorIs(CaptureTerrainInput(registry, oversized), PCG::PCGErrors::SpatialCapacityExceeded);
    }

    TEST_CASE("Terrain input admits a full default tile and rebased origin", "[unit][pcg][terrain]") {
        auto registry = Registry();
        REQUIRE(registry.RegisterDataset(Registration(3, 1'024'000)).HasValue());
        auto candidate = Candidate();
        candidate.originCell = {1, 0, 1};
        candidate.samplesX = 129;
        candidate.samplesZ = 129;
        candidate.samples
            .assign(129U * 129U,
                    {.height = 0.5F, .normal = {0.6F, 0.8F, 0.0F}, .slopeRadians = 0.6435011F, .materialLayer = 3, .exclusionMask = 128});
        const auto captured = CaptureTerrainInput(registry, candidate);
        REQUIRE(captured.HasValue());
        CHECK(captured.Value().Samples().size() == 16'641);
        CHECK(captured.Value().Samples()[0].slopeRadians == 0.6435011F);
        CHECK(captured.Value().Samples()[0].exclusionMask == 128);
        candidate.originCell = {0, 0, 0};
        ErrorIs(CaptureTerrainInput(registry, candidate), PCG::PCGErrors::SpatialInputInvalid);
    }

    TEST_CASE("Terrain input enforces registry capability identity and lifecycle", "[unit][pcg][terrain]") {
        auto noGrant = Registry(false);
        ErrorIs(CaptureTerrainInput(noGrant, Candidate()), Terrain::TerrainErrors::CapabilityUnsupported);
        auto registry = Registry();
        ErrorIs(CaptureTerrainInput(registry, Candidate()), Terrain::TerrainErrors::IdentityUnknown);
        REQUIRE(registry.RegisterDataset(Registration()).HasValue());
        auto wrongContent = Candidate(1, 1, 4);
        ErrorIs(CaptureTerrainInput(registry, wrongContent), PCG::PCGErrors::SpatialInputInvalid);
        const auto retained = CaptureTerrainInput(registry, Candidate()).Value();
        registry.BeginCancellation();
        CHECK(retained.Samples().size() == 4);
        REQUIRE(ValidateTerrainInputCurrent(registry, retained, retained.Revision(), Current()).HasError());
        ErrorIs(CaptureTerrainInput(registry, Candidate()), Terrain::TerrainErrors::RegistryClosed);
        registry.Shutdown();
        CHECK(retained.Samples()[0].height == 0.5F);
    }

    TEST_CASE("Terrain input replacement and all four Terrain revisions fence reuse", "[unit][pcg][terrain]") {
        auto registry = Registry();
        REQUIRE(registry.RegisterDataset(Registration()).HasValue());
        const auto old = CaptureTerrainInput(registry, Candidate()).Value();
        auto changed = old.Revision();
        changed.residency = Number<Terrain::TerrainResidencyRevision>(2);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, changed, Current()), PCG::PCGErrors::SpatialSnapshotStale);
        changed = old.Revision();
        changed.mutation = Number<Terrain::TerrainMutationRevision>(2);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, changed, Current()), PCG::PCGErrors::SpatialSnapshotStale);
        changed = old.Revision();
        changed.capability = Number<Terrain::TerrainCapabilityRevision>(2);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, changed, Current()), PCG::PCGErrors::SpatialSnapshotStale);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, old.Revision(), Current(2)), PCG::PCGErrors::SpatialSnapshotStale);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, old.Revision(), Current(1, 2)), PCG::PCGErrors::SpatialSnapshotStale);
        ErrorIs(ReplaceTerrainInput(registry, old, Candidate(1, 2)), PCG::PCGErrors::SpatialReplacementInvalid);
        ErrorIs(ReplaceTerrainInput(registry, old, Candidate(2, 1)), PCG::PCGErrors::SpatialReplacementInvalid);
        REQUIRE(registry.ReplaceDataset(Registration(4)).HasValue());
        auto next = Candidate(2, 2, 4);
        next.revision.mutation = Number<Terrain::TerrainMutationRevision>(2);
        const auto replacement = ReplaceTerrainInput(registry, old, next);
        REQUIRE(replacement.HasValue());
        CHECK(old.Samples()[0].height == 0.5F);
        ErrorIs(ValidateTerrainInputCurrent(registry, old, old.Revision(), Current()), PCG::PCGErrors::SpatialSnapshotStale);
        REQUIRE(ValidateTerrainInputCurrent(registry, replacement.Value(), next.revision, Current(2)).HasValue());
    }
}  // namespace Horo::PCGTerrain
