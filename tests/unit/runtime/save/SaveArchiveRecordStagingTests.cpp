#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    TEST_CASE("Verified owned records stage with bounded bytes and source provenance", "[runtime][save][archive-reader]") {
        const auto bytes = MakeSaveArchiveReaderFixture();
        const auto archive = SaveArchiveReader{}.Read(bytes);
        REQUIRE(archive.HasValue());
        const auto &manifest = archive.Value().Manifest().participants.front();
        SaveMigrationSource source{.archiveFormatVersion = archive.Value().Preamble().archiveFormatVersion,
                                   .saveSchemaVersion = archive.Value().Manifest().saveSchemaVersion,
                                   .productCompatibility = archive.Value().Header().productCompatibility,
                                   .participants = {{.participant = manifest.participant,
                                                     .schemaVersion = manifest.schemaVersion,
                                                     .required = manifest.required}}};
        const auto oneArchive = V<ArchiveFormatVersion>(1);
        const auto oneSchema = V<SaveSchemaVersion>(1);
        const auto oneProduct = V<ProductSaveCompatibilityVersion>(1);
        const auto oneParticipant = V<ParticipantSchemaVersion>(1);
        const SaveCompatibilityPolicy policy{.archiveVersions = {.direct = {.minimum = oneArchive, .maximum = oneArchive}},
                                             .saveSchemaVersions = {.direct = {.minimum = oneSchema, .maximum = oneSchema}},
                                             .productVersions = {.direct = {.minimum = oneProduct, .maximum = oneProduct}},
                                             .participants = {
                                                 {.participant = manifest.participant,
                                                  .versions = {.direct = {.minimum = oneParticipant, .maximum = oneParticipant}},
                                                  .required = true}}};
        SaveMigrationLimits limits;
        limits.maximumParticipantPayloadBytes = 3;
        limits.maximumTotalPayloadBytes = 3;
        CHECK(RetainVerifiedSaveRecords(archive.Value(), policy, source, limits).HasError());
        CHECK(source.participants.front().records.empty());
        limits.maximumParticipantPayloadBytes = 4;
        limits.maximumTotalPayloadBytes = 4;
        REQUIRE(RetainVerifiedSaveRecords(archive.Value(), policy, source, limits).HasValue());
        REQUIRE(source.participants.front().records.size() == 1);
        const auto &record = source.participants.front().records.front();
        CHECK(record.record == manifest.chunks.front());
        CHECK(record.sourceRecord == record.record);
        CHECK(record.sourceParticipant == manifest.participant);
        CHECK(record.sourceSchemaVersion == manifest.schemaVersion);
        CHECK(record.payload.size() == 4);
    }
}  // namespace
