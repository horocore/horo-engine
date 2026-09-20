#include "Horo/Runtime/Save/SaveArchiveReader.h"
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

    std::vector<std::byte> MakePayload(const std::string &header, const std::string &manifest) {
        const std::array chunk{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        constexpr std::size_t entryCount = 3;
        std::vector<std::byte> payload(SaveArchiveContainerHeaderByteLength + entryCount * SaveArchiveContainerEntryByteLength);
        const std::size_t dataOffset = payload.size();
        payload.insert(payload.end(), reinterpret_cast<const std::byte *>(header.data()),
                       reinterpret_cast<const std::byte *>(header.data() + header.size()));
        const std::uint64_t manifestOffset = payload.size() - dataOffset;
        payload.insert(payload.end(), reinterpret_cast<const std::byte *>(manifest.data()),
                       reinterpret_cast<const std::byte *>(manifest.data() + manifest.size()));
        const std::uint64_t chunkOffset = payload.size() - dataOffset;
        payload.insert(payload.end(), chunk.begin(), chunk.end());

        constexpr std::array<std::byte, 8> containerMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'C'}, std::byte{'T'},
                                                          std::byte{'N'}, std::byte{'R'}, std::byte{'1'}, std::byte{0}};
        std::ranges::copy(containerMagic, payload.begin());
        PutLittleEndian(payload, 8, std::uint32_t{1});
        PutLittleEndian(payload, 12, std::uint32_t{0});
        PutLittleEndian(payload, 16, std::uint64_t{entryCount});
        PutLittleEndian(payload, 24, std::uint32_t{SaveArchiveContainerEntryByteLength});
        PutLittleEndian(payload, 28, std::uint32_t{0});
        return payload;
    }

    void WriteEntry(std::vector<std::byte> &payload, const std::size_t index, const SaveArchiveEntryKind kind, const std::uint64_t offset,
                    const std::span<const std::byte> data, const SaveRecordId record, const SaveParticipantId *owner) {
        const std::size_t entry = EntryOffset(index);
        payload[entry] = static_cast<std::byte>(kind);
        PutLittleEndian(payload, entry + 2, std::uint16_t{0});
        if (owner != nullptr) {
            const auto &ownerText = owner->Value();
            PutLittleEndian(payload, entry + 24, static_cast<std::uint16_t>(ownerText.size()));
            PutString(payload, entry + 28, ownerText);
        }
        if (record.IsValid())
            PutBytes(payload, entry + 8, record.Bytes());
        PutLittleEndian(payload, entry + 124, offset);
        PutLittleEndian(payload, entry + 132, static_cast<std::uint64_t>(data.size()));
        PutLittleEndian(payload, entry + 140, static_cast<std::uint64_t>(data.size()));
        PutLittleEndian(payload, entry + 148, std::uint32_t{1});
        PutBytes(payload, entry + 156, ComputeSha256(data).bytes);
    }

    ArchiveFixture FinalizeArchive(std::vector<std::byte> payload) {
        std::vector<std::byte> preamble(SaveArchivePreambleByteLength);
        constexpr std::array<std::byte, 8> archiveMagic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                        std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
        std::ranges::copy(archiveMagic, preamble.begin());
        PutLittleEndian(preamble, 8, std::uint32_t{1});
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
        auto payload = MakePayload(header, manifest);
        const std::uint64_t manifestOffset = header.size();
        const std::uint64_t chunkOffset = header.size() + manifest.size();

        const auto owner = Manifest().participants.front().participant;
        WriteEntry(payload, 0, SaveArchiveEntryKind::Header, 0, std::as_bytes(std::span{header}), {}, nullptr);
        WriteEntry(payload, 1, SaveArchiveEntryKind::Manifest, manifestOffset, std::as_bytes(std::span{manifest}), {}, nullptr);
        const std::array chunk{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
        WriteEntry(payload, 2, SaveArchiveEntryKind::Chunk, chunkOffset, chunk, Id<SaveRecordId>(20), &owner);
        return FinalizeArchive(std::move(payload));
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
