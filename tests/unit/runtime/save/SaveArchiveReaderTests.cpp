#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveMigration.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    template <typename Value> void PutLittleEndian(std::vector<std::byte> &bytes, const std::size_t offset, const Value value) {
        for (std::size_t index = 0; index < sizeof(Value); ++index)
            bytes[offset + index] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (index * 8U));
    }

    template <typename Value> Value GetLittleEndian(const std::vector<std::byte> &bytes, const std::size_t offset) {
        Value value = 0;
        for (std::size_t index = 0; index < sizeof(Value); ++index)
            value |= static_cast<Value>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
        return value;
    }

    void PutBytes(std::vector<std::byte> &bytes, const std::size_t offset, const std::span<const std::uint8_t> values) {
        for (std::size_t index = 0; index < values.size(); ++index)
            bytes[offset + index] = static_cast<std::byte>(values[index]);
    }

    void PutString(std::vector<std::byte> &bytes, const std::size_t offset, const std::string_view value) {
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[offset + index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
    }

    SaveArchiveHeader Header(std::string engineVersion = "1.2.3-preview.4") {
        return {.slot = Id<SaveGameSlotId>(1),
                .slotGeneration = Id<SlotGenerationId>(2),
                .parentGeneration = std::nullopt,
                .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                .product = Id<ProductStorageId>(3),
                .environment = Id<EnvironmentStorageId>(4),
                .user = Id<LocalUserStorageId>(5),
                .profile = Id<GameProfileId>(6),
                .project = Id<SaveProjectId>(7),
                .world = Id<SaveWorldId>(8),
                .baseScene = Id<SaveBaseSceneId>(9),
                .capturedAtUnixMilliseconds = 1'800'000'000'123ULL,
                .playTimeNanoseconds = 1234,
                .engineVersion = std::move(engineVersion),
                .projectBuildId = "build-1",
                .featureFlags = 0};
    }

    SaveGameManifest Manifest() {
        return {.saveSchemaVersion = V<SaveSchemaVersion>(1),
                .canonicalState = {Digest(7)},
                .participants = {{.participant = SaveParticipantId::Parse("horo.scene.core.v1").Value(),
                                  .schemaVersion = V<ParticipantSchemaVersion>(1),
                                  .required = true,
                                  .chunks = {Id<SaveRecordId>(20)}}}};
    }

    constexpr std::size_t EntryOffset(const std::size_t index) {
        return SaveArchiveContainerHeaderByteLength + index * SaveArchiveContainerEntryByteLength;
    }

    struct ArchiveFixture final {
        std::vector<std::byte> bytes;
        std::size_t chunkEntryOffset{SaveArchivePreambleByteLength + EntryOffset(2)};
    };

    std::vector<std::byte> MakePayload(const std::string &header, const std::string &manifest, const std::span<const std::byte> chunk,
                                       const std::uint32_t version = 1, const std::span<const std::byte> secondChunk = {}) {
        const std::size_t entryCount = secondChunk.empty() ? 3 : 4;
        std::vector<std::byte> payload(SaveArchiveContainerHeaderByteLength + entryCount * SaveArchiveContainerEntryByteLength);
        payload.insert(payload.end(), reinterpret_cast<const std::byte *>(header.data()),
                       reinterpret_cast<const std::byte *>(header.data() + header.size()));
        payload.insert(payload.end(), reinterpret_cast<const std::byte *>(manifest.data()),
                       reinterpret_cast<const std::byte *>(manifest.data() + manifest.size()));
        payload.insert(payload.end(), chunk.begin(), chunk.end());
        payload.insert(payload.end(), secondChunk.begin(), secondChunk.end());

        constexpr std::array<std::byte, 8> containerMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'C'}, std::byte{'T'},
                                                          std::byte{'N'}, std::byte{'R'}, std::byte{'1'}, std::byte{0}};
        std::ranges::copy(containerMagic, payload.begin());
        PutLittleEndian(payload, 8, version);
        PutLittleEndian(payload, 12, std::uint32_t{0});
        PutLittleEndian(payload, 16, std::uint64_t{entryCount});
        PutLittleEndian(payload, 24, std::uint32_t{SaveArchiveContainerEntryByteLength});
        PutLittleEndian(payload, 28, std::uint32_t{0});
        return payload;
    }

    struct EntryEncoding final {
        SaveChunkCodec codec{SaveChunkCodec::Raw};
        std::uint64_t decodedLength{};
        Sha256Digest decodedHash{};
    };

    void WriteEntry(std::vector<std::byte> &payload, const std::size_t index, const SaveArchiveEntryKind kind, const std::uint64_t offset,
                    const std::span<const std::byte> data, const SaveRecordId record, const SaveParticipantId *owner,
                    const EntryEncoding encoding = {}) {
        const std::size_t entry = EntryOffset(index);
        payload[entry] = static_cast<std::byte>(kind);
        PutLittleEndian(payload, entry + 2, static_cast<std::uint16_t>(encoding.codec));
        if (owner != nullptr) {
            const auto &ownerText = owner->Value();
            PutLittleEndian(payload, entry + 24, static_cast<std::uint16_t>(ownerText.size()));
            PutString(payload, entry + 28, ownerText);
        }
        if (record.IsValid())
            PutBytes(payload, entry + 8, record.Bytes());
        PutLittleEndian(payload, entry + 124, offset);
        PutLittleEndian(payload, entry + 132, static_cast<std::uint64_t>(data.size()));
        PutLittleEndian(payload, entry + 140,
                        encoding.decodedLength == 0 ? static_cast<std::uint64_t>(data.size()) : encoding.decodedLength);
        PutLittleEndian(payload, entry + 148, std::uint32_t{1});
        PutBytes(payload, entry + 156, (encoding.decodedLength == 0 ? ComputeSha256(data) : encoding.decodedHash).bytes);
    }

    ArchiveFixture FinalizeArchive(std::vector<std::byte> payload, const std::uint32_t version = 1) {
        std::vector<std::byte> preamble(SaveArchivePreambleByteLength);
        constexpr std::array<std::byte, 8> archiveMagic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                        std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
        std::ranges::copy(archiveMagic, preamble.begin());
        PutLittleEndian(preamble, 8, version);
        PutLittleEndian(preamble, 12, std::uint32_t{0});
        PutLittleEndian(preamble, 16, static_cast<std::uint64_t>(payload.size()));
        PutLittleEndian(preamble, 24, std::uint32_t{SaveArchiveUnsignedTrailerByteLength});
        PutLittleEndian(preamble, 28, std::uint32_t{0});
        const auto finalized = FinalizeSaveArchiveIntegrity(preamble, payload).Value();
        std::vector<std::byte> archive = preamble;
        archive.insert(archive.end(), payload.begin(), payload.end());
        archive.resize(archive.size() + SaveArchiveUnsignedTrailerByteLength);
        PutBytes(archive, archive.size() - SaveArchiveUnsignedTrailerByteLength, finalized.archiveContent.value.bytes);
        return ArchiveFixture{std::move(archive)};
    }

    ArchiveFixture MakeArchive(std::string engineVersion = "1.2.3-preview.4") {
        const auto header = EncodeSaveArchiveHeader(Header(std::move(engineVersion))).Value();
        const auto manifest = EncodeSaveGameManifest(Manifest()).Value();
        const std::array chunk{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        auto payload = MakePayload(header, manifest, chunk);
        const std::uint64_t manifestOffset = header.size();
        const std::uint64_t chunkOffset = header.size() + manifest.size();

        const auto owner = Manifest().participants.front().participant;
        WriteEntry(payload, 0, SaveArchiveEntryKind::Header, 0, std::as_bytes(std::span{header}), {}, nullptr);
        WriteEntry(payload, 1, SaveArchiveEntryKind::Manifest, manifestOffset, std::as_bytes(std::span{manifest}), {}, nullptr);
        WriteEntry(payload, 2, SaveArchiveEntryKind::Chunk, chunkOffset, chunk, Id<SaveRecordId>(20), &owner);
        return FinalizeArchive(std::move(payload));
    }

    ArchiveFixture MakeArchiveWithUnknown(const bool required = false, const std::byte opaqueByte = std::byte{0xa5},
                                          std::string engineVersion = "1.2.3-preview.4", const bool compressed = false) {
        auto manifestValue = Manifest();
        const auto unknownOwner = SaveParticipantId::Parse("project.future.dlc.v1").Value();
        manifestValue.participants.push_back({.participant = unknownOwner,
                                              .schemaVersion = V<ParticipantSchemaVersion>(9),
                                              .required = required,
                                              .chunks = {Id<SaveRecordId>(21)}});
        const auto header = EncodeSaveArchiveHeader(Header(std::move(engineVersion))).Value();
        const auto manifest = EncodeSaveGameManifest(manifestValue).Value();
        const std::array known{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        const std::array rawUnknown{opaqueByte, std::byte{0x00}, std::byte{0xff}};
        std::vector<std::byte> unknown(rawUnknown.begin(), rawUnknown.end());
        EntryEncoding unknownEncoding;
        if (compressed) {
            const std::vector<std::byte> canonical(1'024, opaqueByte);
            const auto encoded = EncodeSaveChunk(canonical, {.preferred = SaveChunkCodec::Deflate, .required = true}, {}).Value();
            unknown = encoded.stored;
            unknownEncoding = {.codec = encoded.codec, .decodedLength = encoded.decodedByteLength, .decodedHash = encoded.decodedHash};
        }
        const std::uint32_t version = compressed ? 2 : 1;
        auto payload = MakePayload(header, manifest, known, version, unknown);
        WriteEntry(payload, 0, SaveArchiveEntryKind::Header, 0, std::as_bytes(std::span{header}), {}, nullptr);
        WriteEntry(payload, 1, SaveArchiveEntryKind::Manifest, header.size(), std::as_bytes(std::span{manifest}), {}, nullptr);
        WriteEntry(payload, 2, SaveArchiveEntryKind::Chunk, header.size() + manifest.size(), known, Id<SaveRecordId>(20),
                   &manifestValue.participants[0].participant);
        WriteEntry(payload, 3, SaveArchiveEntryKind::Chunk, header.size() + manifest.size() + known.size(), unknown, Id<SaveRecordId>(21),
                   &manifestValue.participants[1].participant, unknownEncoding);
        return FinalizeArchive(std::move(payload), version);
    }

    SaveCompatibilityPolicy UnknownPolicy() {
        const auto oneArchive = V<ArchiveFormatVersion>(1);
        const auto oneSchema = V<SaveSchemaVersion>(1);
        const auto oneProduct = V<ProductSaveCompatibilityVersion>(1);
        const auto oneParticipant = V<ParticipantSchemaVersion>(1);
        return {.archiveVersions = {.direct = {.minimum = oneArchive, .maximum = oneArchive}},
                .saveSchemaVersions = {.direct = {.minimum = oneSchema, .maximum = oneSchema}},
                .productVersions = {.direct = {.minimum = oneProduct, .maximum = oneProduct}},
                .participants = {{.participant = Manifest().participants.front().participant,
                                  .versions = {.direct = {.minimum = oneParticipant, .maximum = oneParticipant}},
                                  .required = true}}};
    }

    ArchiveFixture MakeCompressedArchive(const std::span<const std::byte> canonical) {
        const auto encoded = EncodeSaveChunk(canonical, {.preferred = SaveChunkCodec::Deflate, .required = true}, {}).Value();
        const auto header = EncodeSaveArchiveHeader(Header()).Value();
        const auto manifest = EncodeSaveGameManifest(Manifest()).Value();
        auto payload = MakePayload(header, manifest, encoded.stored, 2);
        const auto owner = Manifest().participants.front().participant;
        WriteEntry(payload, 0, SaveArchiveEntryKind::Header, 0, std::as_bytes(std::span{header}), {}, nullptr);
        WriteEntry(payload, 1, SaveArchiveEntryKind::Manifest, header.size(), std::as_bytes(std::span{manifest}), {}, nullptr);
        WriteEntry(payload, 2, SaveArchiveEntryKind::Chunk, header.size() + manifest.size(), encoded.stored, Id<SaveRecordId>(20), &owner,
                   {.codec = encoded.codec, .decodedLength = encoded.decodedByteLength, .decodedHash = encoded.decodedHash});
        return FinalizeArchive(std::move(payload), 2);
    }

    void Rehash(ArchiveFixture &fixture) {
        const auto payload = std::span<const std::byte>{fixture.bytes}.subspan(SaveArchivePreambleByteLength,
                                                                               fixture.bytes.size() - SaveArchivePreambleByteLength -
                                                                                   SaveArchiveUnsignedTrailerByteLength);
        const auto preamble = std::span<const std::byte>{fixture.bytes}.first(SaveArchivePreambleByteLength);
        const auto finalized = FinalizeSaveArchiveIntegrity(preamble, payload).Value();
        PutBytes(fixture.bytes, fixture.bytes.size() - SaveArchiveUnsignedTrailerByteLength, finalized.archiveContent.value.bytes);
    }

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
