#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Runtime;
using namespace Horo::Runtime::Test;

namespace {
    std::vector<std::byte> Hex(std::string_view value) {
        std::vector<std::byte> bytes;
        for (std::size_t index = 0; index < value.size(); index += 2) {
            const auto digit = [](char c) {
                return c <= '9' ? c - '0' : c - 'a' + 10;
            };
            bytes.push_back(static_cast<std::byte>(digit(value[index]) * 16 + digit(value[index + 1])));
        }
        return bytes;
    }
}  // namespace

TEST_CASE("Scene schema2 canonical aggregate matches an independent length-delimited golden and excludes publication metadata",
          "[save][canonical][content]") {
    SaveArchiveHeader header{.project = Id<SaveProjectId>(1), .world = Id<SaveWorldId>(2), .baseScene = Id<SaveBaseSceneId>(3)};
    const std::array payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    const std::array records{SaveSceneCanonicalRecord{Id<SaveRecordId>(4), std::span<const std::byte>{payload}}};
    const std::array owners{
        SaveSceneCanonicalParticipant{SaveParticipantId::Parse("project.state").Value(), V<ParticipantSchemaVersion>(1), records}};
    const auto golden = Hex("10000000000000000000000000000000000000011000000000000000000000000000000000000002100000000000000000000000000000"
                            "0000000003020000000000000001000000390000000d00000070726f6a6563742e737461746501000000010000001c0000001000000000"
                            "0000000000000000000000000000040103000000112233");
    auto encoded = EncodeSaveSceneCanonicalState(header, owners);
    REQUIRE(encoded.HasValue());
    CHECK(std::ranges::equal(encoded.Value().Bytes(), golden));
    const auto digest = Hex("df3425be098e2d3bf8dfa8341d064a97f3aa7d39d70111884c2bbff5006cf6f2");
    const auto hash = ComputeCanonicalStateHash(encoded.Value().Bytes());
    CHECK(std::ranges::equal(std::as_bytes(std::span{hash.value.bytes}), digest));
    header.slot = Id<SaveGameSlotId>(99);
    header.capturedAtUnixMilliseconds = 12345;
    header.engineVersion = "new-release";
    auto repeated = EncodeSaveSceneCanonicalState(header, owners);
    REQUIRE(repeated.HasValue());
    CHECK(std::ranges::equal(repeated.Value().Bytes(), golden));
    header.baseScene = Id<SaveBaseSceneId>(9);
    REQUIRE(EncodeSaveSceneCanonicalState(header, owners).HasValue());
    CHECK(ComputeCanonicalStateHash(EncodeSaveSceneCanonicalState(header, owners).Value().Bytes()) != hash);
    CHECK(EncodeSaveSceneCanonicalState(header, owners, 2).HasError());
}

TEST_CASE("Canonical layout rejects missing self duplicate unknown tag self opaque newer version and every truncation",
          "[save][canonical][content]") {
    std::vector entries{SaveSceneCanonicalLayoutEntry{Id<SaveRecordId>(1), SaveSceneCanonicalRepresentation::Opaque},
                        SaveSceneCanonicalLayoutEntry{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known}};
    std::ranges::sort(entries, {}, &SaveSceneCanonicalLayoutEntry::record);
    auto encoded = EncodeSaveSceneCanonicalLayout(entries);
    REQUIRE(encoded.HasValue());
    auto decoded = DecodeSaveSceneCanonicalLayout(encoded.Value().Bytes());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value() == entries);
    const auto bytes = encoded.Value().Bytes();
    CHECK(bytes.size() == 58);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        REQUIRE(DecodeSaveSceneCanonicalLayout(bytes.first(length)).HasError());
    auto forged = entries;
    forged.push_back(entries.back());
    CHECK(EncodeSaveSceneCanonicalLayout(forged).HasError());
    forged = entries;
    forged.front().representation = static_cast<SaveSceneCanonicalRepresentation>(99);
    CHECK(EncodeSaveSceneCanonicalLayout(forged).HasError());
    forged = {{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Opaque}};
    CHECK(EncodeSaveSceneCanonicalLayout(forged).HasError());
    CHECK(EncodeSaveSceneCanonicalLayout(std::array{entries.front()}).HasError());
    std::vector<std::byte> newer(bytes.begin(), bytes.end());
    newer.front() = std::byte{2};
    CHECK(DecodeSaveSceneCanonicalLayout(newer).HasError());
}

namespace {
    SaveArchiveHeader CompleteHeader() {
        return {.slot = Id<SaveGameSlotId>(1),
                .slotGeneration = Id<SlotGenerationId>(2),
                .productCompatibility = V<ProductSaveCompatibilityVersion>(1),
                .product = Id<ProductStorageId>(3),
                .environment = Id<EnvironmentStorageId>(4),
                .user = Id<LocalUserStorageId>(5),
                .profile = Id<GameProfileId>(6),
                .project = Id<SaveProjectId>(7),
                .world = Id<SaveWorldId>(8),
                .baseScene = Id<SaveBaseSceneId>(9),
                .capturedAtUnixMilliseconds = 1,
                .engineVersion = "fixture-1",
                .projectBuildId = "fixture-1"};
    }

    Result<FinalizedSaveArchive> LayoutArchive(const SaveChunkCodec codec, const SaveSceneCanonicalRepresentation representation,
                                               const bool required = false, const bool wrongHash = false, const bool incomplete = false,
                                               const std::uint32_t schema = 2, const std::uint32_t container = 2,
                                               const std::uint64_t declaredDecodedLength = 1) {
        const auto header = CompleteHeader();
        const auto owner = SaveParticipantId::Parse("project.state").Value();
        const auto version = V<ParticipantSchemaVersion>(1);
        const std::array bytes{std::byte{0x11}};
        std::vector tags{SaveSceneCanonicalLayoutEntry{Id<SaveRecordId>(1), representation},
                         SaveSceneCanonicalLayoutEntry{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalRepresentation::Known}};
        if (incomplete)
            tags.erase(tags.begin());
        auto encodedLayout = EncodeSaveSceneCanonicalLayout(tags);
        REQUIRE(encodedLayout.HasValue());
        std::vector<std::byte> layoutBytes(encodedLayout.Value().Bytes().begin(), encodedLayout.Value().Bytes().end());
        const SaveChunkDirectoryEntry data{Id<SaveRecordId>(1),   owner, 0,     bytes.size(),
                                           declaredDecodedLength, 1,     codec, ComputeSha256(bytes)};
        const SaveSceneCanonicalOpaque opaque{codec,          data.storedByteLength, data.decodedByteLength,
                                              data.alignment, data.decodedHash,      bytes};
        const std::array layoutRecords{SaveSceneCanonicalRecord{SaveSceneCanonicalLayoutRecord(), std::span<const std::byte>{layoutBytes}}};
        const std::array dataRecords{
            SaveSceneCanonicalRecord{data.record, representation == SaveSceneCanonicalRepresentation::Known
                                                      ? decltype(SaveSceneCanonicalRecord::payload){std::span<const std::byte>{bytes}}
                                                      : decltype(SaveSceneCanonicalRecord::payload){opaque}}};
        const std::array participants{SaveSceneCanonicalParticipant{SaveSceneCanonicalLayoutParticipant(), version, layoutRecords},
                                      SaveSceneCanonicalParticipant{owner, version, dataRecords}};
        auto state = EncodeSaveSceneCanonicalState(header, participants);
        REQUIRE(state.HasValue());
        auto hash = ComputeCanonicalStateHash(state.Value().Bytes());
        if (wrongHash)
            hash.value.bytes[0] ^= 1;
        const SaveGameManifest manifest{V<SaveSchemaVersion>(schema),
                                        hash,
                                        {{SaveSceneCanonicalLayoutParticipant(), version, true, {SaveSceneCanonicalLayoutRecord()}},
                                         {owner, version, required, {data.record}}}};
        const std::array chunks{PreservedSaveChunk{data, {bytes.begin(), bytes.end()}},
                                PreservedSaveChunk{{SaveSceneCanonicalLayoutRecord(), SaveSceneCanonicalLayoutParticipant(), 0,
                                                    layoutBytes.size(), layoutBytes.size(), 1, SaveChunkCodec::Raw,
                                                    ComputeSha256(layoutBytes)},
                                                   std::move(layoutBytes)}};
        return SaveArchiveContainerWriter::Write(header, manifest, chunks, V<ArchiveFormatVersion>(container));
    }
}  // namespace

TEST_CASE("Schema2 authenticates unsupported optional codec frames and preserves their classification when codec support exists",
          "[save][canonical][content]") {
    for (const auto codec : {static_cast<SaveChunkCodec>(99), SaveChunkCodec::Raw}) {
        auto archive = LayoutArchive(codec, SaveSceneCanonicalRepresentation::Opaque);
        if (archive.HasError()) {
            INFO("opaque fixture codec=" << static_cast<unsigned>(codec) << " container=2: " << archive.ErrorValue().code.Value());
            std::string diagnostics;
            for (const auto &diagnostic : archive.ErrorValue().diagnostics)
                diagnostics += diagnostic.path + ": " + diagnostic.message + "; ";
            INFO("reader diagnostics: " << diagnostics);
            REQUIRE(archive.HasValue());
        }
        REQUIRE(archive.HasValue());
        auto read = SaveArchiveReader{}.Read(archive.Value().Archive().bytes);
        REQUIRE(read.HasValue());
        auto classification = ValidateSaveSceneCanonicalArchive(read.Value());
        REQUIRE(classification.HasValue());
        CHECK(classification.Value().front().representation == SaveSceneCanonicalRepresentation::Opaque);
        if (codec != SaveChunkCodec::Raw) {
            const auto selected = read.Value().SelectChunk(Id<SaveRecordId>(1));
            REQUIRE(selected.HasError());
            CHECK(selected.ErrorValue().code.Value() == "save.archive.codec_unsupported");
            const auto repeated = read.Value().SelectChunk(Id<SaveRecordId>(1));
            REQUIRE(repeated.HasError());
            CHECK(repeated.ErrorValue().code.Value() == "save.archive.codec_unsupported");
            SaveChunkDirectory directory{.integrityAlgorithm = {},
                                         .payloadByteLength = read.Value().Payload().size(),
                                         .entries = {read.Value().Directory().Entries().begin(), read.Value().Directory().Entries().end()}};
            const auto ordinary = ValidateSaveChunkDirectory(std::move(directory), read.Value().Manifest());
            REQUIRE(ordinary.HasError());
            CHECK(ordinary.ErrorValue().code.Value() == "save.archive.codec_unsupported");
        }
    }
    auto knownUnsupported = LayoutArchive(static_cast<SaveChunkCodec>(99), SaveSceneCanonicalRepresentation::Known);
    REQUIRE(knownUnsupported.HasValue());
    auto knownRead = SaveArchiveReader{}.Read(knownUnsupported.Value().Archive().bytes);
    REQUIRE(knownRead.HasValue());
    CHECK(ValidateSaveSceneCanonicalArchive(knownRead.Value()).HasError());
}

TEST_CASE("Schema2 rejects forged required opaque tags incomplete coverage and changed source hashes before semantic admission",
          "[save][canonical][content]") {
    for (const auto options : {std::array{true, false, false}, std::array{false, true, false}, std::array{false, false, true}}) {
        auto archive = LayoutArchive(SaveChunkCodec::Raw, SaveSceneCanonicalRepresentation::Opaque, options[0], options[1], options[2]);
        REQUIRE(archive.HasValue());
        auto read = SaveArchiveReader{}.Read(archive.Value().Archive().bytes);
        REQUIRE(read.HasValue());
        CHECK(ValidateSaveSceneCanonicalArchive(read.Value()).HasError());
        CHECK(ValidatedSaveSceneCanonicalPreservation::Create(read.Value()).HasError());
    }
}

TEST_CASE("Opaque codec admission requires version two schema two and an actual optional manifest owner", "[save][canonical][content]") {
    const auto codec = static_cast<SaveChunkCodec>(99);
    for (const auto options : {std::array<std::uint32_t, 3>{1, 2, 2}, {0, 1, 2}, {0, 2, 1}}) {
        const auto rejected =
            LayoutArchive(codec, SaveSceneCanonicalRepresentation::Opaque, options[0] != 0, false, false, options[1], options[2]);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "save.archive.codec_unsupported");
    }
    const auto oversized = LayoutArchive(codec, SaveSceneCanonicalRepresentation::Opaque, false, false, false, 2, 2, 65);
    REQUIRE(oversized.HasError());
    CHECK(oversized.ErrorValue().code.Value() == "save.archive.decompression_limit_exceeded");
}
