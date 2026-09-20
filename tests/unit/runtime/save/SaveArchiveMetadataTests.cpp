#include "Horo/Runtime/Save/SaveArchiveMetadata.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    SaveArchiveMetadataLimits Limits() {
        SaveArchiveMetadataLimits limits;
        limits.supportedFeatureFlagsMask = 0x3U;
        return limits;
    }

    SaveArchiveHeader Header() {
        return {.slot = Id<SaveGameSlotId>(1),
                .slotGeneration = Id<SlotGenerationId>(2),
                .parentGeneration = Id<SlotGenerationId>(3),
                .productCompatibility = V<ProductSaveCompatibilityVersion>(4),
                .product = Id<ProductStorageId>(5),
                .environment = Id<EnvironmentStorageId>(6),
                .user = Id<LocalUserStorageId>(7),
                .profile = Id<GameProfileId>(8),
                .project = Id<SaveProjectId>(9),
                .world = Id<SaveWorldId>(10),
                .baseScene = Id<SaveBaseSceneId>(11),
                .capturedAtUnixMilliseconds = 1'800'000'000'123ULL,
                .playTimeNanoseconds = 9'876'543'210ULL,
                .engineVersion = "1.2.3-preview.4",
                .projectBuildId = "build-2026-09-06.1",
                .featureFlags = 0x1U};
    }

    SaveGameManifest Manifest() {
        return {.saveSchemaVersion = V<SaveSchemaVersion>(3),
                .canonicalState = {ComputeSha256({})},
                .participants = StandardParticipants(2, 5)};
    }

    template <typename Tag>
    SaveVersionSupport<Tag> Support(const std::uint32_t direct, const std::optional<std::uint32_t> migration = std::nullopt) {
        SaveVersionSupport<Tag> support{.direct = {.minimum = V<SaveVersion<Tag>>(direct), .maximum = V<SaveVersion<Tag>>(direct)}};
        if (migration)
            support.migrationSource =
                SaveVersionRange<Tag>{.minimum = V<SaveVersion<Tag>>(*migration), .maximum = V<SaveVersion<Tag>>(*migration)};
        return support;
    }

    SaveCompatibilityPolicy Policy() {
        return {.archiveVersions = Support<ArchiveFormatVersionTag>(1),
                .saveSchemaVersions = Support<SaveSchemaVersionTag>(3, 2),
                .productVersions = Support<ProductSaveCompatibilityVersionTag>(4),
                .participants = {{.participant = SaveParticipantId::Parse("horo.scene.core.v1").Value(),
                                  .versions = Support<ParticipantSchemaVersionTag>(2),
                                  .required = true},
                                 {.participant = SaveParticipantId::Parse("project.gameplay.v1").Value(),
                                  .versions = Support<ParticipantSchemaVersionTag>(5, 4),
                                  .required = false}},
                .supportedFeatureFlagsMask = 0x3U};
    }

    TEST_CASE("Save archive header has a canonical exact-shape round trip", "[runtime][save][archive-metadata]") {
        const SaveArchiveHeader header = Header();
        const auto encoded = EncodeSaveArchiveHeader(header, Limits());
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value().starts_with(R"({"baseScene":)"));
        const auto decoded = DecodeSaveArchiveHeader(encoded.Value(), Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == header);
        CHECK(EncodeSaveArchiveHeader(decoded.Value(), Limits()).Value() == encoded.Value());
    }

    TEST_CASE("Save header rejects missing duplicate unknown and ill-typed fields", "[runtime][save][archive-metadata]") {
        const std::string valid = EncodeSaveArchiveHeader(Header(), Limits()).Value();
        auto missing = valid;
        const auto field = missing.find(R"("world":)");
        missing.erase(field, missing.size() - field - 1);
        missing.back() = '}';
        CHECK(DecodeSaveArchiveHeader(missing, Limits()).HasError());

        auto duplicate = valid;
        duplicate.insert(1, R"("slot":"00000000-0000-0000-0000-000000000001",)");
        CHECK(DecodeSaveArchiveHeader(duplicate, Limits()).HasError());

        auto unknown = valid;
        unknown.insert(1, R"("unknown":0,)");
        CHECK(DecodeSaveArchiveHeader(unknown, Limits()).HasError());

        auto wrongType = valid;
        const auto timestamp = wrongType.find("1800000000123");
        wrongType.replace(timestamp, 13, R"("1800000000123")");
        CHECK(DecodeSaveArchiveHeader(wrongType, Limits()).HasError());
    }

    TEST_CASE("Save header enforces identity text feature and byte limits", "[runtime][save][archive-metadata]") {
        auto header = Header();
        header.slot = {};
        CHECK(ValidateSaveArchiveHeader(header, Limits()).HasError());
        header = Header();
        header.parentGeneration = header.slotGeneration;
        CHECK(ValidateSaveArchiveHeader(header, Limits()).HasError());
        header = Header();
        header.featureFlags = 0x4U;
        CHECK(ValidateSaveArchiveHeader(header, Limits()).HasError());
        header = Header();
        header.engineVersion.assign(Limits().maximumTextBytes + 1, 'x');
        CHECK(ValidateSaveArchiveHeader(header, Limits()).HasError());
        header = Header();
        header.engineVersion.clear();
        CHECK(ValidateSaveArchiveHeader(header, Limits()).HasError());

        const std::string encoded = EncodeSaveArchiveHeader(Header(), Limits()).Value();
        auto tiny = Limits();
        tiny.maximumHeaderBytes = encoded.size() - 1;
        CHECK(DecodeSaveArchiveHeader(encoded, tiny).HasError());
    }

    TEST_CASE("Save manifest has a canonical stable-order round trip", "[runtime][save][archive-metadata]") {
        const SaveGameManifest manifest = Manifest();
        const auto encoded = EncodeSaveGameManifest(manifest, Limits());
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value().starts_with(R"({"canonicalStateHash":)"));
        const auto decoded = DecodeSaveGameManifest(encoded.Value(), Limits());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == manifest);
        CHECK(EncodeSaveGameManifest(decoded.Value(), Limits()).Value() == encoded.Value());
    }

    TEST_CASE("Save manifest rejects duplicate missing unordered and oversized data", "[runtime][save][archive-metadata]") {
        const std::string valid = EncodeSaveGameManifest(Manifest(), Limits()).Value();
        auto duplicateKey = valid;
        duplicateKey.insert(1, R"("saveSchemaVersion":3,)");
        CHECK(DecodeSaveGameManifest(duplicateKey, Limits()).HasError());

        auto manifest = Manifest();
        std::swap(manifest.participants[0], manifest.participants[1]);
        CHECK(ValidateSaveGameManifest(manifest, Limits()).HasError());
        manifest = Manifest();
        manifest.participants[0].chunks.push_back(manifest.participants[0].chunks.front());
        CHECK(ValidateSaveGameManifest(manifest, Limits()).HasError());
        manifest = Manifest();
        manifest.participants[1].chunks = {manifest.participants[0].chunks.front()};
        CHECK(ValidateSaveGameManifest(manifest, Limits()).HasError());
        manifest = Manifest();
        manifest.participants[0].chunks.clear();
        CHECK(ValidateSaveGameManifest(manifest, Limits()).HasError());

        auto bounded = Limits();
        bounded.maximumParticipants = 1;
        CHECK(DecodeSaveGameManifest(valid, bounded).HasError());
        bounded = Limits();
        bounded.maximumTotalChunks = 2;
        bounded.maximumChunksPerParticipant = 2;
        CHECK(DecodeSaveGameManifest(valid, bounded).HasError());

        bounded = Limits();
        bounded.maximumTotalChunks = std::numeric_limits<std::size_t>::max();
        CHECK(ValidateSaveGameManifest(Manifest(), bounded).HasValue());
    }

    TEST_CASE("Compatibility preflight distinguishes direct migration and rejection", "[runtime][save][compatibility]") {
        auto header = Header();
        auto manifest = Manifest();
        auto policy = Policy();
        const auto archiveV1 = V<ArchiveFormatVersion>(1);
        CHECK(EvaluateSaveCompatibility(archiveV1, header, manifest, policy).disposition == SaveCompatibilityDisposition::DirectRead);

        manifest.saveSchemaVersion = V<SaveSchemaVersion>(2);
        CHECK(EvaluateSaveCompatibility(archiveV1, header, manifest, policy).disposition ==
              SaveCompatibilityDisposition::MigrationRequired);

        manifest = Manifest();
        manifest.participants[0].schemaVersion = V<ParticipantSchemaVersion>(99);
        const auto unsupported = EvaluateSaveCompatibility(archiveV1, header, manifest, policy);
        CHECK(unsupported.disposition == SaveCompatibilityDisposition::Rejected);
        CHECK(unsupported.reason == SaveCompatibilityReason::UnsupportedParticipantSchema);
        REQUIRE(unsupported.participant);
        CHECK(*unsupported.participant == manifest.participants[0].participant);
    }

    TEST_CASE("Compatibility preflight rejects missing and unknown required participants", "[runtime][save][compatibility]") {
        const auto archiveV1 = V<ArchiveFormatVersion>(1);
        auto manifest = Manifest();
        auto policy = Policy();
        manifest.participants.erase(manifest.participants.begin());
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, policy).reason ==
              SaveCompatibilityReason::MissingRequiredParticipant);

        manifest = Manifest();
        policy.participants.erase(policy.participants.begin());
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, policy).reason ==
              SaveCompatibilityReason::UnknownRequiredParticipant);

        manifest = Manifest();
        manifest.participants[1].required = false;
        policy = Policy();
        policy.participants.erase(policy.participants.begin() + 1);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, policy).disposition == SaveCompatibilityDisposition::DirectRead);
    }

    TEST_CASE("Compatibility preflight covers independent root and participant axes", "[runtime][save][compatibility]") {
        const auto archiveV1 = V<ArchiveFormatVersion>(1);
        const auto checkReason = [&](const ArchiveFormatVersion archive, const SaveArchiveHeader &header, const SaveGameManifest &manifest,
                                     const SaveCompatibilityPolicy &policy, const SaveCompatibilityReason reason) {
            const auto decision = EvaluateSaveCompatibility(archive, header, manifest, policy);
            CHECK(decision.disposition == SaveCompatibilityDisposition::Rejected);
            CHECK(decision.reason == reason);
        };

        checkReason(V<ArchiveFormatVersion>(9), Header(), Manifest(), Policy(), SaveCompatibilityReason::UnsupportedArchiveVersion);
        auto manifest = Manifest();
        manifest.saveSchemaVersion = V<SaveSchemaVersion>(9);
        checkReason(archiveV1, Header(), manifest, Policy(), SaveCompatibilityReason::UnsupportedSaveSchema);
        auto header = Header();
        header.productCompatibility = V<ProductSaveCompatibilityVersion>(9);
        checkReason(archiveV1, header, Manifest(), Policy(), SaveCompatibilityReason::UnsupportedProductVersion);
        header = Header();
        header.featureFlags = 0x4U;
        checkReason(archiveV1, header, Manifest(), Policy(), SaveCompatibilityReason::UnsupportedFeature);

        manifest = Manifest();
        manifest.participants[1].schemaVersion = V<ParticipantSchemaVersion>(4);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, Policy()).disposition ==
              SaveCompatibilityDisposition::MigrationRequired);
    }

    TEST_CASE("Compatibility preflight rejects malformed release policies", "[runtime][save][compatibility]") {
        const auto archiveV1 = V<ArchiveFormatVersion>(1);
        auto policy = Policy();
        std::swap(policy.participants[0], policy.participants[1]);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), Manifest(), policy).reason == SaveCompatibilityReason::InvalidMetadata);

        policy = Policy();
        policy.participants.push_back(policy.participants.back());
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), Manifest(), policy).reason == SaveCompatibilityReason::InvalidMetadata);

        policy = Policy();
        policy.archiveVersions.direct.minimum = V<ArchiveFormatVersion>(2);
        policy.archiveVersions.direct.maximum = V<ArchiveFormatVersion>(1);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), Manifest(), policy).reason == SaveCompatibilityReason::InvalidMetadata);

        policy = Policy();
        policy.archiveVersions.migrationSource = policy.archiveVersions.direct;
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), Manifest(), policy).reason == SaveCompatibilityReason::InvalidMetadata);

        policy = Policy();
        policy.saveSchemaVersions.direct = {.minimum = V<SaveSchemaVersion>(3), .maximum = V<SaveSchemaVersion>(4)};
        policy.saveSchemaVersions.migrationSource =
            SaveVersionRange<SaveSchemaVersionTag>{.minimum = V<SaveSchemaVersion>(1), .maximum = V<SaveSchemaVersion>(2)};
        auto manifest = Manifest();
        manifest.saveSchemaVersion = V<SaveSchemaVersion>(2);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, policy).disposition ==
              SaveCompatibilityDisposition::MigrationRequired);

        policy.saveSchemaVersions.migrationSource->maximum = V<SaveSchemaVersion>(3);
        CHECK(EvaluateSaveCompatibility(archiveV1, Header(), manifest, policy).reason == SaveCompatibilityReason::InvalidMetadata);
    }
}  // namespace
