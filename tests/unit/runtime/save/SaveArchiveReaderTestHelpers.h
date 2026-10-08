#pragma once

#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
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

namespace Horo::Runtime::ArchiveReaderTest {
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

    inline void PutBytes(std::vector<std::byte> &bytes, const std::size_t offset, const std::span<const std::uint8_t> values) {
        for (std::size_t index = 0; index < values.size(); ++index)
            bytes[offset + index] = static_cast<std::byte>(values[index]);
    }

    inline void PutString(std::vector<std::byte> &bytes, const std::size_t offset, const std::string_view value) {
        for (std::size_t index = 0; index < value.size(); ++index)
            bytes[offset + index] = static_cast<std::byte>(static_cast<unsigned char>(value[index]));
    }

    inline SaveArchiveHeader Header(std::string engineVersion = "1.2.3-preview.4") {
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

    inline SaveGameManifest Manifest() {
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

    inline std::vector<std::byte> MakePayload(const std::string &header, const std::string &manifest,
                                              const std::span<const std::byte> chunk, const std::uint32_t version = 1,
                                              const std::span<const std::byte> secondChunk = {}) {
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

    inline void WriteEntry(std::vector<std::byte> &payload, const std::size_t index, const SaveArchiveEntryKind kind,
                           const std::uint64_t offset, const std::span<const std::byte> data, const SaveRecordId record,
                           const SaveParticipantId *owner, const EntryEncoding encoding = {}) {
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

    inline ArchiveFixture FinalizeArchive(std::vector<std::byte> payload, const std::uint32_t version = 1) {
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

    inline ArchiveFixture MakeArchive(std::string engineVersion = "1.2.3-preview.4") {
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

    inline ArchiveFixture MakeArchiveWithUnknown(const bool required = false, const std::byte opaqueByte = std::byte{0xa5},
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

    inline SaveCompatibilityPolicy UnknownPolicy() {
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

    inline ArchiveFixture MakeCompressedArchive(const std::span<const std::byte> canonical) {
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

    inline void Rehash(ArchiveFixture &fixture) {
        const auto payload = std::span<const std::byte>{fixture.bytes}.subspan(SaveArchivePreambleByteLength,
                                                                               fixture.bytes.size() - SaveArchivePreambleByteLength -
                                                                                   SaveArchiveUnsignedTrailerByteLength);
        const auto preamble = std::span<const std::byte>{fixture.bytes}.first(SaveArchivePreambleByteLength);
        const auto finalized = FinalizeSaveArchiveIntegrity(preamble, payload).Value();
        PutBytes(fixture.bytes, fixture.bytes.size() - SaveArchiveUnsignedTrailerByteLength, finalized.archiveContent.value.bytes);
    }

}  // namespace Horo::Runtime::ArchiveReaderTest
