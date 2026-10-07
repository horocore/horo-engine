#include "SaveArchiveReaderTestHelpers.h"

using namespace Horo;
using namespace Horo::Runtime;
using namespace Horo::Runtime::Test;
using namespace Horo::Runtime::ArchiveReaderTest;

namespace {
    /** @brief Copies exact admitted stored frames without interpreting or changing their directory evidence. */
    std::vector<PreservedSaveChunk> CopyStoredChunks(const ValidatedSaveArchive &archive) {
        std::vector<PreservedSaveChunk> chunks;
        for (const auto &entry : archive.Directory().Entries()) {
            const auto stored =
                archive.Payload().subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.storedByteLength));
            chunks.push_back({entry, {stored.begin(), stored.end()}});
        }
        return chunks;
    }
}  // namespace

TEST_CASE("Production container writer matches independent v1 wire fixture and rejects incomplete inventory", "[runtime][save][archive]") {
    const auto fixture = MakeArchive();
    auto source = SaveArchiveReader{}.Read(fixture.bytes);
    REQUIRE(source.HasValue());
    auto chunks = CopyStoredChunks(source.Value());
    auto rebuilt = SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                                     source.Value().Preamble().archiveFormatVersion);
    REQUIRE(rebuilt.HasValue());
    CHECK(std::ranges::equal(rebuilt.Value().Bytes(), fixture.bytes));
    REQUIRE(SaveArchiveReader{}.Read(rebuilt.Value().Archive().bytes).HasValue());
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), {},
                                            source.Value().Preamble().archiveFormatVersion)
              .HasError());
    SaveArchiveReaderLimits limits;
    limits.maximumArchiveBytes = fixture.bytes.size() - 1;
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                            source.Value().Preamble().archiveFormatVersion, limits)
              .HasError());
    auto duplicate = chunks;
    duplicate.push_back(chunks.back());
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), duplicate,
                                            source.Value().Preamble().archiveFormatVersion)
              .HasError());
    auto empty = chunks;
    empty.front().storedBytes.clear();
    empty.front().entry.storedByteLength = 0;
    empty.front().entry.decodedByteLength = 0;
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), empty,
                                            source.Value().Preamble().archiveFormatVersion)
              .HasError());
    std::uint64_t rawWork{};
    for (const auto &chunk : chunks)
        rawWork += chunk.storedBytes.size();
    SaveArchiveReaderLimits workBoundary;
    workBoundary.maximumReadWorkBytes = rawWork + 3 * fixture.bytes.size();
    REQUIRE(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                              source.Value().Preamble().archiveFormatVersion, workBoundary)
                .HasValue());
    --workBoundary.maximumReadWorkBytes;
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                            source.Value().Preamble().archiveFormatVersion, workBoundary)
              .HasError());
    chunks.front().storedBytes.front() ^= std::byte{1};
    auto corrupt = SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                                     source.Value().Preamble().archiveFormatVersion);
    REQUIRE(corrupt.HasError());
    CHECK(corrupt.ErrorValue().code.Value() == "save.archive.chunk_hash_mismatch");
}

TEST_CASE("Production v2 container preserves verified compressed unknown bytes and codec through reader round trip",
          "[runtime][save][archive]") {
    const auto fixture = MakeArchiveWithUnknown(false, std::byte{0xa5}, "1.2.3-preview.4", true);
    auto source = SaveArchiveReader{}.Read(fixture.bytes);
    REQUIRE(source.HasValue());
    auto chunks = CopyStoredChunks(source.Value());
    auto rebuilt = SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                                     source.Value().Preamble().archiveFormatVersion);
    REQUIRE(rebuilt.HasValue());
    CHECK(std::ranges::equal(rebuilt.Value().Bytes(), fixture.bytes));
    auto destination = SaveArchiveReader{}.Read(rebuilt.Value().Archive().bytes);
    REQUIRE(destination.HasValue());
    auto policy = UnknownPolicy();
    policy.archiveVersions.direct.maximum = V<ArchiveFormatVersion>(2);
    CHECK(VerifyUnknownDataRoundTrip(source.Value(), destination.Value(), policy, 1024).HasValue());
}

namespace {
    /** @brief Independent wire fixture with a header padded to align two unchanged raw records. */
    ArchiveFixture AlignedOptionalArchive() {
        const auto original = MakeArchiveWithUnknown();
        const auto admitted = SaveArchiveReader{}.Read(original.bytes).Value();
        auto header = EncodeSaveArchiveHeader(admitted.Header()).Value();
        const auto manifest = EncodeSaveGameManifest(admitted.Manifest()).Value();
        const auto known = admitted.SelectChunk(Id<SaveRecordId>(20)).Value().value();
        const auto unknown = admitted.SelectChunk(Id<SaveRecordId>(21)).Value().value();
        const auto firstOffset =
            SaveArchiveContainerHeaderByteLength + 4 * SaveArchiveContainerEntryByteLength + header.size() + manifest.size();
        header.append((8 - firstOffset % 8) % 8, ' ');
        auto payload = MakePayload(header, manifest, known, 1, unknown);
        WriteEntry(payload, 0, SaveArchiveEntryKind::Header, 0, std::as_bytes(std::span{header}), {}, nullptr);
        WriteEntry(payload, 1, SaveArchiveEntryKind::Manifest, header.size(), std::as_bytes(std::span{manifest}), {}, nullptr);
        WriteEntry(payload, 2, SaveArchiveEntryKind::Chunk, header.size() + manifest.size(), known, Id<SaveRecordId>(20),
                   &admitted.Manifest().participants[0].participant);
        WriteEntry(payload, 3, SaveArchiveEntryKind::Chunk, header.size() + manifest.size() + known.size(), unknown, Id<SaveRecordId>(21),
                   &admitted.Manifest().participants[1].participant);
        PutLittleEndian(payload, EntryOffset(2) + 148, std::uint32_t{8});
        PutLittleEndian(payload, EntryOffset(3) + 148, std::uint32_t{4});
        return FinalizeArchive(std::move(payload));
    }
}  // namespace

TEST_CASE("Production container preserves aligned opaque bytes with exact padded metadata digests", "[runtime][save][archive]") {
    auto fixture = AlignedOptionalArchive();
    auto source = SaveArchiveReader{}.Read(fixture.bytes);
    REQUIRE(source.HasValue());
    auto chunks = CopyStoredChunks(source.Value());
    auto rebuilt = SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                                     source.Value().Preamble().archiveFormatVersion);
    REQUIRE(rebuilt.HasValue());
    CHECK(std::ranges::equal(rebuilt.Value().Bytes(), fixture.bytes));
    auto destination = SaveArchiveReader{}.Read(rebuilt.Value().Archive().bytes);
    REQUIRE(destination.HasValue());
    CHECK(destination.Value().Directory().Entries()[0].alignment == 8);
    CHECK(destination.Value().Directory().Entries()[1].alignment == 4);
    CHECK(VerifyUnknownDataRoundTrip(source.Value(), destination.Value(), UnknownPolicy(), 1024).HasValue());
    chunks[1].entry.alignment = 8;  // Four-byte preceding record makes these contiguous alignment constraints incompatible.
    CHECK(SaveArchiveContainerWriter::Write(source.Value().Header(), source.Value().Manifest(), chunks,
                                            source.Value().Preamble().archiveFormatVersion)
              .HasError());
    const auto headerData = SaveArchivePreambleByteLength + SaveArchiveContainerHeaderByteLength + 4 * SaveArchiveContainerEntryByteLength;
    fixture.bytes[headerData] ^= std::byte{1};
    Rehash(fixture);  // A valid outer digest cannot conceal a changed metadata entry digest.
    CHECK(SaveArchiveReader{}.Read(fixture.bytes).HasError());
}
