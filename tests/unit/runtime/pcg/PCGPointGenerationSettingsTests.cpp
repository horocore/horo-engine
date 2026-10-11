#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "PCGPointGenerationInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

namespace Horo::PCG {
    namespace {
        SpatialElementId Source(const std::uint64_t value) {
            const auto source = SpatialElementId::Create(value);
            REQUIRE(source.HasValue());
            return source.Value();
        }

        template <typename T> T Stable(const std::uint64_t value) {
            const auto result = T::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        PCGSpatialSnapshot GenerationSpatial() {
            PCGSpatialSnapshotCandidate candidate;
            candidate.snapshot = Stable<SpatialSnapshotId>(1);
            candidate.provenance = {Stable<SpatialProviderId>(2), Stable<SpatialSourceId>(3), Stable<SpatialRevision>(4)};
            candidate.bounds = {{-2.0F, -2.0F, -2.0F}, {2.0F, 2.0F, 2.0F}};
            candidate.coverage = PCGSpatialCoverage::Complete;
            candidate.volumes = {PCGBoxVolume{Source(9), {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}}},
                                 PCGSphereVolume{Source(10), {}, 1.0F}};
            candidate.grids = {PCGGrid{Source(8), {}, {1.0F, 1.0F, 1.0F}, {2, 2, 2}}};
            const auto spatial = CapturePCGSpatialSnapshot(std::move(candidate));
            REQUIRE(spatial.HasValue());
            return spatial.Value();
        }

        void AddField(std::vector<PCGAttributeDescriptor> &attributes, const std::string_view key,
                      const PCGAttributeType type = PCGAttributeType::UnsignedInteger) {
            const auto id = PCGAttributeKey::Create(key);
            REQUIRE(id.HasValue());
            attributes.push_back({id.Value(), type});
        }

        std::shared_ptr<const PCGPointSchema> Schema(const std::vector<PCGAttributeDescriptor> &attributes) {
            const auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, attributes});
            REQUIRE(schema.HasValue());
            return std::make_shared<const PCGPointSchema>(schema.Value());
        }

        template <typename T> void Error(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("PCG grid settings preserve the version-one exact source bytes", "[pcg][generation][settings]") {
        const auto encoded = EncodePCGPointGenerationSettings(PCGGridGenerationSettings{Source(0x0102030405060708ULL)});
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value() == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
        Error(EncodePCGPointGenerationSettings(PCGGridGenerationSettings{}), PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG jitter settings canonicalize signed zero and reject schema skew", "[pcg][generation][settings]") {
        const auto positive = EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{Source(8), 0.0});
        const auto negative = EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{Source(8), -0.0});
        REQUIRE(positive.HasValue());
        REQUIRE(negative.HasValue());
        CHECK(positive.Value() == negative.Value());
        REQUIRE(positive.Value().size() == 20);
        CHECK(positive.Value() == std::vector<std::uint8_t>{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0});
        const auto decoded = detail::DecodeJitterSettings(positive.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().grid == Source(8));
        CHECK(decoded.Value().fraction == 0.0);
        auto malformed = positive.Value();
        malformed[3] = 2;
        Error(detail::DecodeJitterSettings(malformed), PCGErrors::CpuEvaluationUnsupported);
        malformed = positive.Value();
        malformed[12] = 0x80;
        Error(detail::DecodeJitterSettings(malformed), PCGErrors::CpuEvaluationInvalid);
        malformed = positive.Value();
        malformed.push_back(0);
        Error(detail::DecodeJitterSettings(malformed), PCGErrors::CpuEvaluationInvalid);
        Error(EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{Source(8), 1.01}), PCGErrors::CpuEvaluationInvalid);
        Error(EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{Source(8), -0.01}), PCGErrors::CpuEvaluationInvalid);
        Error(EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{Source(8), std::numeric_limits<double>::quiet_NaN()}),
              PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG stratified settings and checked counts retain every axis", "[pcg][generation][settings]") {
        const auto encoded = EncodePCGPointGenerationSettings(PCGStratifiedGenerationSettings{Source(9), {2, 3, 4}});
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value().size() == 24);
        CHECK(encoded.Value() == std::vector<std::uint8_t>{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4});
        const auto decoded = detail::DecodeStratifiedSettings(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().strata == std::array<std::uint32_t, 3>{2, 3, 4});
        const auto charge = detail::ChargePointGeneration(decoded.Value(), 24, 7);
        REQUIRE(charge.HasValue());
        CHECK(charge.Value().pointVisits == 24);
        CHECK(charge.Value().snapshotElementVisits == 14);
        CHECK(charge.Value().scratchBytes == 0);
        Error(detail::ChargePointGeneration(decoded.Value(), 23, 7), PCGErrors::CpuEvaluationCapacityExceeded);
        Error(EncodePCGPointGenerationSettings(PCGStratifiedGenerationSettings{Source(9), {2, 0, 4}}), PCGErrors::CpuEvaluationInvalid);
        const auto maximum = std::numeric_limits<std::uint32_t>::max();
        Error(detail::ChargePointGeneration(PCGStratifiedGenerationSettings{Source(9), {maximum, maximum, maximum}},
                                            std::numeric_limits<std::size_t>::max(), 7),
              PCGErrors::PointSizeOverflow);
    }

    TEST_CASE("PCG Poisson admission charges worst-case attempts pairs and temporary positions", "[pcg][generation][settings]") {
        const PCGPoissonGenerationSettings settings{Source(9), 5, 20, 0.25};
        const auto encoded = EncodePCGPointGenerationSettings(settings);
        REQUIRE(encoded.HasValue());
        REQUIRE(encoded.Value().size() == 28);
        CHECK(encoded.Value() ==
              std::vector<std::uint8_t>{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 5, 0, 0, 0, 20, 0x3f, 0xd0, 0, 0, 0, 0, 0, 0});
        const auto decoded = detail::DecodePoissonSettings(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().volume == Source(9));
        CHECK(decoded.Value().count == 5);
        CHECK(decoded.Value().attempts == 20);
        CHECK(decoded.Value().minimumDistance == 0.25);
        const auto charge = detail::ChargePointGeneration(decoded.Value(), 5, 7);
        REQUIRE(charge.HasValue());
        CHECK(charge.Value().pointVisits == 125);
        CHECK(charge.Value().scratchBytes == 5 * sizeof(Math::Vec3));
        CHECK(charge.Value().snapshotElementVisits == 14);
        Error(detail::ChargePointGeneration(settings, 4, 7), PCGErrors::CpuEvaluationCapacityExceeded);
        Error(detail::ChargePointGeneration(PCGPoissonGenerationSettings{Source(9), 5, 4, 0.25}, 5, 7),
              PCGErrors::GenerationAttemptsExhausted);
        const auto empty = detail::ChargePointGeneration(PCGPoissonGenerationSettings{Source(9), 0, 0, 1.0}, 0, 7);
        REQUIRE(empty.HasValue());
        CHECK(empty.Value().pointVisits == 0);
        CHECK(empty.Value().scratchBytes == 0);
        for (const double distance : {0.0, -1.0, std::numeric_limits<double>::infinity()})
            Error(EncodePCGPointGenerationSettings(PCGPoissonGenerationSettings{Source(9), 5, 20, distance}),
                  PCGErrors::CpuEvaluationInvalid);
    }

    TEST_CASE("PCG generator admission proves exact captured source and complete identity schema", "[pcg][generation][admission]") {
        const auto spatial = GenerationSpatial();
        std::vector<PCGAttributeDescriptor> attributes;
        PCGPointOutputBound output{0, Stable<PinId>(1), Schema(attributes), 8};
        CHECK(detail::ValidatePointGeneration(PCGGridGenerationSettings{Source(8)}, spatial, output).HasValue());
        Error(detail::ValidatePointGeneration(PCGJitterGenerationSettings{Source(8), 1.0}, spatial, output),
              PCGErrors::GenerationIdentityInvalid);
        AddField(attributes, PCGSourceSampleAttribute);
        output.schema = Schema(attributes);
        Error(detail::ValidatePointGeneration(PCGGridGenerationSettings{Source(8)}, spatial, output), PCGErrors::GenerationIdentityInvalid);
        for (const auto key : PCGSourceRootAttributes)
            AddField(attributes, key);
        output.schema = Schema(attributes);
        CHECK(detail::ValidatePointGeneration(PCGJitterGenerationSettings{Source(8), 1.0}, spatial, output).HasValue());
        // SnapshotGrid v1 preserves its existing missing-source diagnostic; the new typed jitter contract owns SpatialInputInvalid.
        Error(detail::ValidatePointGeneration(PCGGridGenerationSettings{Source(99)}, spatial, output), PCGErrors::CpuEvaluationInvalid);
        Error(detail::ValidatePointGeneration(PCGJitterGenerationSettings{Source(99), 1.0}, spatial, output),
              PCGErrors::SpatialInputInvalid);
        Error(detail::ValidatePointGeneration(PCGStratifiedGenerationSettings{Source(10), {2, 2, 2}}, spatial, output),
              PCGErrors::CpuEvaluationUnsupported);
        Error(detail::ValidatePointGeneration(PCGPoissonGenerationSettings{Source(9), 2, 100, 10.0}, spatial, output),
              PCGErrors::GenerationDensityImpossible);
        Error(detail::ValidatePointGeneration(PCGPoissonGenerationSettings{Source(9), 2, 1, 0.1}, spatial, output),
              PCGErrors::GenerationAttemptsExhausted);
        // Subnormal finite separation must saturate the density proof rather than overflow a cell count.
        CHECK(detail::ValidatePointGeneration(PCGPoissonGenerationSettings{Source(9), 8, 100, std::numeric_limits<double>::denorm_min()},
                                              spatial, output)
                  .HasValue());
        output.maximumPoints = 7;
        Error(detail::ValidatePointGeneration(PCGGridGenerationSettings{Source(8)}, spatial, output),
              PCGErrors::CpuEvaluationCapacityExceeded);
        attributes.back().type = PCGAttributeType::Scalar;
        output.schema = Schema(attributes);
        Error(detail::ValidatePointGeneration(PCGJitterGenerationSettings{Source(8), 1.0}, spatial, output),
              PCGErrors::GenerationIdentityInvalid);
    }
}  // namespace Horo::PCG
