#include "SaveArchiveReaderTestHelpers.h"

using namespace Horo::Runtime::ArchiveReaderTest;

namespace {
    TEST_CASE("Bounded reader admits a finalized archive and retains explicit storage ownership", "[runtime][save][archive-reader]") {
        auto fixture = MakeArchive();
        auto owned = std::make_shared<const std::vector<std::byte>>(fixture.bytes);
        auto result = SaveArchiveReader{}.Read(owned);
        REQUIRE(result.HasValue());
        CHECK(result.Value().Header().slot == Id<SaveGameSlotId>(1));
        CHECK(result.Value().Manifest().participants.size() == 1);
        CHECK(result.Value().Directory().Entries().size() == 1);
        const auto selected = result.Value().SelectChunk(Id<SaveRecordId>(20));
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value());
        CHECK(selected.Value()->size() == 4);
        CHECK(result.Value().Signature().algorithm == SaveArchiveSignatureAlgorithm::None);
    }

    TEST_CASE("Unknown required content rejects before restore with its stable owner identity", "[runtime][save][archive-reader]") {
        const auto bytes = MakeArchiveWithUnknown(true).bytes;
        const auto archive = SaveArchiveReader{}.Read(bytes);
        REQUIRE(archive.HasValue());
        const auto report = archive.Value().InspectUnknownData(UnknownPolicy(), 1024);
        REQUIRE(report.HasError());
        CHECK(report.ErrorValue().message ==
              "Save cannot be restored: required module or content participant 'project.future.dlc.v1' is unavailable.");
    }

    TEST_CASE("Unknown optional bytes and integrity metadata survive a changed archive envelope", "[runtime][save][archive-reader]") {
        const auto sourceBytes = MakeArchiveWithUnknown().bytes;
        const auto destinationBytes = MakeArchiveWithUnknown(false, std::byte{0xa5}, "next-build").bytes;
        auto source = SaveArchiveReader{}.Read(sourceBytes);
        auto destination = SaveArchiveReader{}.Read(destinationBytes);
        REQUIRE(source.HasValue());
        REQUIRE(destination.HasValue());
        const auto policy = UnknownPolicy();
        auto report = source.Value().InspectUnknownData(policy, 3);
        REQUIRE(report.HasValue());
        REQUIRE(report.Value().preserved.size() == 1);
        CHECK(report.Value().preserved[0].storedBytes == std::vector<std::byte>{std::byte{0xa5}, std::byte{0}, std::byte{0xff}});
        CHECK(report.Value().preserved[0].entry.decodedHash == ComputeSha256(report.Value().preserved[0].storedBytes));
        CHECK(VerifyUnknownDataRoundTrip(source.Value(), destination.Value(), policy, 3).HasValue());

        const auto missingBytes = MakeArchive().bytes;
        const auto missing = SaveArchiveReader{}.Read(missingBytes);
        REQUIRE(missing.HasValue());
        CHECK(VerifyUnknownDataRoundTrip(source.Value(), missing.Value(), policy, 3).HasError());

        const auto changedBytes = MakeArchiveWithUnknown(false, std::byte{0xa6}).bytes;
        auto changed = SaveArchiveReader{}.Read(changedBytes);
        REQUIRE(changed.HasValue());
        const auto rejected = VerifyUnknownDataRoundTrip(source.Value(), changed.Value(), policy, 3);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().message.find("project.future.dlc.v1") != std::string::npos);
        CHECK(source.Value().InspectUnknownData(policy, 2).HasError());
    }

    TEST_CASE("Compressed unknown optional storage retains exact codec bytes and digest", "[runtime][save][archive-reader]") {
        const auto sourceBytes = MakeArchiveWithUnknown(false, std::byte{0xa5}, "build-one", true).bytes;
        const auto destinationBytes = MakeArchiveWithUnknown(false, std::byte{0xa5}, "build-two", true).bytes;
        const auto source = SaveArchiveReader{}.Read(sourceBytes);
        const auto destination = SaveArchiveReader{}.Read(destinationBytes);
        REQUIRE(source.HasValue());
        REQUIRE(destination.HasValue());
        auto policy = UnknownPolicy();
        policy.archiveVersions.direct.maximum = V<ArchiveFormatVersion>(2);
        const auto report = source.Value().InspectUnknownData(policy, 1024);
        REQUIRE(report.HasValue());
        REQUIRE(report.Value().preserved.size() == 1);
        CHECK(report.Value().preserved.front().entry.codec == SaveChunkCodec::Deflate);
        CHECK(report.Value().preserved.front().entry.decodedByteLength == 1'024);
        CHECK(report.Value().preserved.front().storedBytes.size() < 1'024);
        CHECK(VerifyUnknownDataRoundTrip(source.Value(), destination.Value(), policy, 1024).HasValue());
    }

    TEST_CASE("Unknown optional drops require an explicit valid release policy", "[runtime][save][archive-reader]") {
        const auto bytes = MakeArchiveWithUnknown().bytes;
        auto archive = SaveArchiveReader{}.Read(bytes);
        REQUIRE(archive.HasValue());
        auto policy = UnknownPolicy();
        policy.droppableUnknownParticipants = {SaveParticipantId::Parse("project.future.dlc.v1").Value()};
        const auto report = archive.Value().InspectUnknownData(policy, 0);
        REQUIRE(report.HasValue());
        CHECK(report.Value().preserved.empty());
        CHECK(report.Value().dropped == policy.droppableUnknownParticipants);
        policy.droppableUnknownParticipants.push_back(policy.droppableUnknownParticipants.front());
        CHECK(archive.Value().InspectUnknownData(policy, 3).HasError());

        policy.droppableUnknownParticipants.pop_back();
        auto malformed = MakeArchiveWithUnknown();
        const auto relative = GetLittleEndian<std::uint64_t>(malformed.bytes, SaveArchivePreambleByteLength + EntryOffset(3) + 124);
        const auto storedOffset = SaveArchivePreambleByteLength + SaveArchiveContainerHeaderByteLength +
                                  4 * SaveArchiveContainerEntryByteLength + static_cast<std::size_t>(relative);
        malformed.bytes[storedOffset] = std::byte{0xa6};
        Rehash(malformed);
        const auto admitted = SaveArchiveReader{}.Read(malformed.bytes);
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().InspectUnknownData(policy, 0).HasError());
    }

    TEST_CASE("Newer optional module schema is treated as opaque unless required by another owner", "[runtime][save][archive-reader]") {
        const auto bytes = MakeArchiveWithUnknown().bytes;
        const auto archive = SaveArchiveReader{}.Read(bytes);
        REQUIRE(archive.HasValue());
        auto policy = UnknownPolicy();
        const auto future = SaveParticipantId::Parse("project.future.dlc.v1").Value();
        const auto v1 = V<ParticipantSchemaVersion>(1);
        policy.participants.push_back({.participant = future, .versions = {.direct = {.minimum = v1, .maximum = v1}}, .required = false});
        const auto retained = archive.Value().InspectUnknownData(policy, 3);
        REQUIRE(retained.HasValue());
        CHECK(retained.Value().preserved.size() == 1);
        policy.participants.front().requiredDependencies = {future};
        const auto blocked = archive.Value().InspectUnknownData(policy, 3);
        REQUIRE(blocked.HasError());
        CHECK(blocked.ErrorValue().message.find("project.future.dlc.v1") != std::string::npos);
    }

    TEST_CASE("Verified unknown records attach to detached migration state atomically", "[runtime][save][archive-reader]") {
        const auto bytes = MakeArchiveWithUnknown().bytes;
        auto archive = SaveArchiveReader{}.Read(bytes);
        REQUIRE(archive.HasValue());
        SaveMigrationSource source{.archiveFormatVersion = archive.Value().Preamble().archiveFormatVersion,
                                   .saveSchemaVersion = archive.Value().Manifest().saveSchemaVersion,
                                   .productCompatibility = archive.Value().Header().productCompatibility,
                                   .archiveBytes = bytes};
        for (const auto &entry : archive.Value().Manifest().participants)
            source.participants.push_back(
                {.participant = entry.participant, .schemaVersion = entry.schemaVersion, .required = entry.required});
        CHECK(RetainUnknownSaveData(archive.Value(), UnknownPolicy(), source, 2).HasError());
        CHECK(source.participants.back().preservedChunks.empty());
        REQUIRE(RetainUnknownSaveData(archive.Value(), UnknownPolicy(), source, 3).HasValue());
        REQUIRE(source.participants.back().preservedChunks.size() == 1);
        CHECK(source.participants.back().preservedChunks.front().storedBytes ==
              std::vector<std::byte>{std::byte{0xa5}, std::byte{0}, std::byte{0xff}});
        source.saveSchemaVersion = V<SaveSchemaVersion>(2);
        CHECK(RetainUnknownSaveData(archive.Value(), UnknownPolicy(), source, 3).HasError());
    }

    TEST_CASE("Archive reader reserves cumulative validation work before hashing", "[runtime][save][archive-reader]") {
        const auto fixture = MakeArchive();
        SaveArchiveReaderLimits limits;
        limits.maximumReadWorkBytes = fixture.bytes.size() * 3 - 1;
        const auto exhausted = SaveArchiveReader{limits}.Read(fixture.bytes);
        REQUIRE(exhausted.HasError());
        CHECK(exhausted.ErrorValue().code.Value() == SaveErrors::ArchiveFramingLimitExceeded.code.Value());
        REQUIRE(exhausted.ErrorValue().diagnostics.size() == 1);
        CHECK(exhausted.ErrorValue().diagnostics.front().code.Value() == "save.archive.limit.read_work");
        limits.maximumReadWorkBytes = fixture.bytes.size() * 3;
        CHECK(SaveArchiveReader{limits}.Read(fixture.bytes).HasValue());
        limits.maximumReadWorkBytes += 4;
        auto validated = SaveArchiveReader{limits}.Read(fixture.bytes);
        REQUIRE(validated.HasValue());
        CHECK(validated.Value().SelectChunk(Id<SaveRecordId>(20)).HasValue());
        const auto repeated = validated.Value().SelectChunk(Id<SaveRecordId>(20));
        REQUIRE(repeated.HasError());
        CHECK(repeated.ErrorValue().code.Value() == SaveErrors::ArchiveFramingLimitExceeded.code.Value());
        CHECK(repeated.ErrorValue().diagnostics.front().code.Value() == "save.archive.limit.read_work");

        limits = {};
        limits.maximumExpansionRatio = std::numeric_limits<std::uint64_t>::max();
        CHECK(SaveArchiveReader{limits}.Read(fixture.bytes).HasError());
    }

    TEST_CASE("Bounded reader rejects truncation, trailing bytes, overlap, gaps, and arithmetic overflow",
              "[runtime][save][archive-reader]") {
        auto fixture = MakeArchive();
        fixture.bytes.pop_back();
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchivePayloadTruncated.code.Value());

        fixture = MakeArchive();
        fixture.bytes.push_back(std::byte{});
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).HasError());

        fixture = MakeArchive();
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 124, std::uint64_t{1});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveDirectoryInvalid.code.Value());

        fixture = MakeArchive();
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 124, std::uint64_t{2});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveDirectoryInvalid.code.Value());

        fixture = MakeArchive();
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 132, std::numeric_limits<std::uint64_t>::max());
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).HasError());
    }

    TEST_CASE("Bounded reader rejects unsupported codec, unsafe owner links, nesting, and decompression abuse",
              "[runtime][save][archive-reader]") {
        auto fixture = MakeArchive();
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 2, std::uint16_t{99});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveCodecUnsupported.code.Value());

        fixture = MakeArchive();
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 24, std::uint16_t{6});
        PutString(fixture.bytes, fixture.chunkEntryOffset + 28, "../x/y");
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveUnsafeReference.code.Value());

        fixture = MakeArchive();
        fixture.bytes[fixture.chunkEntryOffset + 28] = std::byte{0x1f};
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveUnsafeReference.code.Value());

        fixture = MakeArchive();
        auto limits = SaveArchiveReaderLimits{};
        limits.metadata.maximumNestingDepth = 1;
        CHECK(SaveArchiveReader{limits}.Read(fixture.bytes).HasError());

        fixture = MakeArchive();
        limits = {};
        limits.chunks.maximumDecodedChunkBytes = 2;
        CHECK(SaveArchiveReader{limits}.Read(fixture.bytes).ErrorValue().code.Value() ==
              SaveErrors::ArchiveDecompressionLimitExceeded.code.Value());
    }

    TEST_CASE("Version two decodes a compressed chunk and preserves canonical logical identity", "[runtime][save][archive-reader]") {
        std::vector<std::byte> canonical(4'096);
        for (std::size_t index = 0; index < canonical.size(); ++index)
            canonical[index] = static_cast<std::byte>(index % 64);
        auto fixture = MakeCompressedArchive(canonical);
        auto admitted = SaveArchiveReader{}.Read(fixture.bytes);
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().Preamble().archiveFormatVersion.Value() == 2);
        REQUIRE(admitted.Value().Directory().Entries()[0].codec == SaveChunkCodec::Deflate);
        const auto decoded = admitted.Value().SelectChunk(Id<SaveRecordId>(20));
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value());
        CHECK(*decoded.Value() == canonical);
        CHECK(ComputeCanonicalStateHash(*decoded.Value()) == ComputeCanonicalStateHash(canonical));

        PutLittleEndian(fixture.bytes, 8, std::uint32_t{1});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).HasError());
        PutLittleEndian(fixture.bytes, SaveArchivePreambleByteLength + 8, std::uint32_t{1});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveCodecUnsupported.code.Value());
    }

    TEST_CASE("Version two never admits unsupported metadata codecs even for optional opaque storage", "[runtime][save][archive-reader]") {
        const std::vector<std::byte> canonical(1'024, std::byte{0x42});
        for (const std::size_t entry : {std::size_t{0}, std::size_t{1}}) {
            auto fixture = MakeCompressedArchive(canonical);
            PutLittleEndian(fixture.bytes, SaveArchivePreambleByteLength + EntryOffset(entry) + 2, std::uint16_t{99});
            Rehash(fixture);
            const auto rejected = SaveArchiveReader{}.Read(fixture.bytes);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == SaveErrors::ArchiveCodecUnsupported.code.Value());
        }
    }

    TEST_CASE("Compressed chunk admission rejects hostile declared expansion before decode allocation", "[runtime][save][archive-reader]") {
        std::vector<std::byte> canonical(1'024);
        for (std::size_t index = 0; index < canonical.size(); ++index)
            canonical[index] = static_cast<std::byte>(index % 64);
        auto fixture = MakeCompressedArchive(canonical);
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 140, std::uint64_t{1ULL << 32U});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() ==
              SaveErrors::ArchiveDecompressionLimitExceeded.code.Value());

        fixture = MakeCompressedArchive(canonical);
        const auto stored = GetLittleEndian<std::uint64_t>(fixture.bytes, fixture.chunkEntryOffset + 132);
        PutLittleEndian(fixture.bytes, fixture.chunkEntryOffset + 140, stored * 65);
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() ==
              SaveErrors::ArchiveDecompressionLimitExceeded.code.Value());
    }

    TEST_CASE("Compressed chunk corruption and newer archive versions fail without returning payload", "[runtime][save][archive-reader]") {
        std::vector<std::byte> canonical(1'024);
        for (std::size_t index = 0; index < canonical.size(); ++index)
            canonical[index] = static_cast<std::byte>(index % 64);
        auto fixture = MakeCompressedArchive(canonical);
        const auto relative = GetLittleEndian<std::uint64_t>(fixture.bytes, fixture.chunkEntryOffset + 124);
        const auto storedOffset = SaveArchivePreambleByteLength + SaveArchiveContainerHeaderByteLength +
                                  3 * SaveArchiveContainerEntryByteLength + static_cast<std::size_t>(relative);
        fixture.bytes[storedOffset] = std::byte{};
        Rehash(fixture);
        auto admitted = SaveArchiveReader{}.Read(fixture.bytes);
        REQUIRE(admitted.HasValue());
        const auto selected = admitted.Value().SelectChunk(Id<SaveRecordId>(20));
        REQUIRE(selected.HasError());
        CHECK(selected.ErrorValue().code.Value() == SaveErrors::ArchiveChunkDecodeFailed.code.Value());

        fixture = MakeCompressedArchive(canonical);
        PutLittleEndian(fixture.bytes, SaveArchivePreambleByteLength + EntryOffset(0) + 2,
                        static_cast<std::uint16_t>(SaveChunkCodec::Deflate));
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveCodecUnsupported.code.Value());

        fixture = MakeCompressedArchive(canonical);
        PutLittleEndian(fixture.bytes, 8, std::uint32_t{3});
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::VersionUnsupportedNewer.code.Value());
    }

    TEST_CASE("Bounded reader rejects extension records and malformed UTF-8 before exposure", "[runtime][save][archive-reader]") {
        auto fixture = MakeArchive();
        fixture.bytes[fixture.chunkEntryOffset - SaveArchiveContainerEntryByteLength] =
            static_cast<std::byte>(SaveArchiveEntryKind::Extension);
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).ErrorValue().code.Value() == SaveErrors::ArchiveExtensionInvalid.code.Value());

        fixture = MakeArchive();
        const auto headerEntryOffset = SaveArchivePreambleByteLength + EntryOffset(0);
        const std::size_t headerDataOffset = headerEntryOffset + 124;
        const auto relativeOffset = GetLittleEndian<std::uint64_t>(fixture.bytes, headerDataOffset);
        const auto absoluteHeader = SaveArchivePreambleByteLength + SaveArchiveContainerHeaderByteLength +
                                    3 * SaveArchiveContainerEntryByteLength + static_cast<std::size_t>(relativeOffset);
        fixture.bytes[absoluteHeader] = std::byte{static_cast<unsigned char>(0xc0)};
        Rehash(fixture);
        CHECK(SaveArchiveReader{}.Read(fixture.bytes).HasError());
    }

    TEST_CASE("Bounded reader defers chunk checksum verification until selection", "[runtime][save][archive-reader]") {
        auto fixture = MakeArchive();
        const auto relativeOffset = GetLittleEndian<std::uint64_t>(fixture.bytes, fixture.chunkEntryOffset + 124);
        const auto absoluteChunk = SaveArchivePreambleByteLength + SaveArchiveContainerHeaderByteLength +
                                   3 * SaveArchiveContainerEntryByteLength + static_cast<std::size_t>(relativeOffset);
        fixture.bytes[absoluteChunk] = static_cast<std::byte>(std::to_integer<std::uint8_t>(fixture.bytes[absoluteChunk]) ^ 1U);
        Rehash(fixture);

        auto admitted = SaveArchiveReader{}.Read(fixture.bytes);
        REQUIRE(admitted.HasValue());
        const auto selected = admitted.Value().SelectChunk(Id<SaveRecordId>(20));
        REQUIRE(selected.HasError());
        CHECK(selected.ErrorValue().code.Value() == SaveErrors::ArchiveChunkHashMismatch.code.Value());
    }
}  // namespace

namespace Horo::Runtime::Test {
    std::vector<std::byte> MakeSaveArchiveReaderFixture() {
        return MakeArchive().bytes;
    }
}  // namespace Horo::Runtime::Test
