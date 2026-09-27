#include "Horo/Runtime/Save/SaveArchiveFraming.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    std::vector<std::byte> Payload() {
        return {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5},
                std::byte{6}, std::byte{7}, std::byte{8}, std::byte{9}};
    }

    SaveGameManifest Manifest() {
        return {.saveSchemaVersion = V<SaveSchemaVersion>(1),
                .canonicalState = {ComputeSha256({})},
                .participants = StandardParticipants(1, 1)};
    }

    SaveChunkDirectory Directory(const std::span<const std::byte> payload) {
        const auto manifest = Manifest();
        const auto entry = [&](const std::size_t index, const std::uint64_t offset, const std::uint64_t length,
                               const SaveParticipantId owner) {
            return SaveChunkDirectoryEntry{.record = Id<SaveRecordId>(static_cast<std::uint8_t>(20 + index)),
                                           .owner = owner,
                                           .offset = offset,
                                           .storedByteLength = length,
                                           .decodedByteLength = length,
                                           .alignment = 1,
                                           .codec = SaveChunkCodec::Raw,
                                           .decodedHash = ComputeSha256(payload.subspan(offset, length))};
        };
        return {.payloadByteLength = payload.size(),
                .entries = {entry(0, 0, 3, manifest.participants[0].participant), entry(1, 3, 2, manifest.participants[0].participant),
                            entry(2, 5, 4, manifest.participants[1].participant)}};
    }

    TEST_CASE("Save chunk directory validates metadata without reading payload bytes", "[runtime][save][framing]") {
        const auto payload = Payload();
        CHECK(ValidateSaveChunkDirectory(Directory(payload), Manifest()).HasValue());
    }

    TEST_CASE("Selective chunk access verifies only the requested raw payload", "[runtime][save][framing]") {
        auto payload = Payload();
        const auto directory = ValidateSaveChunkDirectory(Directory(payload), Manifest()).Value();
        const auto selected = SelectSaveChunkPayload(payload, directory, Id<SaveRecordId>(21));
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value());
        CHECK(selected.Value()->size() == 2);
        CHECK(selected.Value()->front() == std::byte{4});

        const auto unknown = SelectSaveChunkPayload(payload, directory, Id<SaveRecordId>(99));
        REQUIRE(unknown.HasValue());
        CHECK_FALSE(unknown.Value());

        payload[3] = std::byte{99};
        CHECK(SelectSaveChunkPayload(payload, directory, Id<SaveRecordId>(21)).HasError());
    }

    TEST_CASE("Codec inventory and per-chunk policy keep metadata and small records raw", "[runtime][save][compression]") {
        const auto installed = InstalledSaveChunkCodecs();
        REQUIRE(installed.size() == 2);
        CHECK(installed[0].codec == SaveChunkCodec::Raw);
        CHECK(installed[1].codec == SaveChunkCodec::Deflate);
        CHECK(installed[1].minimumLevel == 1);
        CHECK(installed[1].maximumLevel == 9);
        CHECK_FALSE(installed[1].supportsDictionaries);

        const auto small = Payload();
        CHECK(EncodeSaveChunk(small, {.preferred = SaveChunkCodec::Deflate}, {}).Value().codec == SaveChunkCodec::Raw);
        CHECK(EncodeSaveChunk(small, {.preferred = SaveChunkCodec::Deflate, .metadata = true}, {}).Value().codec == SaveChunkCodec::Raw);
        CHECK(EncodeSaveChunk(small, {.preferred = SaveChunkCodec::Deflate, .required = true}, {}).ErrorValue().code.Value() ==
              SaveErrors::ArchiveCompressionPolicyInvalid.code.Value());
        CHECK(EncodeSaveChunk(small, {.preferred = static_cast<SaveChunkCodec>(99)}, {}).Value().codec == SaveChunkCodec::Raw);
        CHECK(EncodeSaveChunk(small, {.preferred = static_cast<SaveChunkCodec>(99), .required = true}, {}).ErrorValue().code.Value() ==
              SaveErrors::ArchiveCodecUnsupported.code.Value());
        CHECK(EncodeSaveChunk(small, {.preferred = SaveChunkCodec::Deflate, .level = 10}, {}).ErrorValue().code.Value() ==
              SaveErrors::ArchiveCompressionPolicyInvalid.code.Value());
    }

    TEST_CASE("Compression cannot alter the canonical hash and rejects bounded output policy", "[runtime][save][compression]") {
        std::vector<std::byte> canonical(2'048);
        for (std::size_t index = 0; index < canonical.size(); ++index)
            canonical[index] = static_cast<std::byte>(index % 64);
        const auto logical = ComputeCanonicalStateHash(canonical);
        const auto encoded = EncodeSaveChunk(canonical, {.preferred = SaveChunkCodec::Deflate, .required = true}, {});
        REQUIRE(encoded.HasValue());
        CHECK(encoded.Value().codec == SaveChunkCodec::Deflate);
        CHECK(encoded.Value().decodedHash == ComputeSha256(canonical));
        CHECK(ComputeCanonicalStateHash(encoded.Value().stored) != logical);

        auto limits = SaveChunkDirectoryLimits{};
        limits.maximumDecodedChunkBytes = 512;
        CHECK(EncodeSaveChunk(canonical, {}, limits).ErrorValue().code.Value() ==
              SaveErrors::ArchiveDecompressionLimitExceeded.code.Value());
        limits = {};
        limits.maximumStoredChunkBytes = 16;
        CHECK(EncodeSaveChunk(canonical, {.preferred = SaveChunkCodec::Deflate, .required = true}, limits).HasError());
    }

    TEST_CASE("Compressed chunks admit an independent zlib stream only at its exact decoded length", "[runtime][save][compression]") {
        constexpr std::array compressed{std::byte{0x78}, std::byte{0x9c}, std::byte{0x63}, std::byte{0x64},
                                        std::byte{0x62}, std::byte{0x06}, std::byte{0x00}, std::byte{0x00},
                                        std::byte{0x0d}, std::byte{0x00}, std::byte{0x07}};
        const auto raw = Payload();
        std::vector<std::byte> payload{compressed.begin(), compressed.end()};
        payload.insert(payload.end(), raw.begin() + 3, raw.end());
        auto directory = Directory(raw);
        directory.payloadByteLength = payload.size();
        directory.entries[0].codec = SaveChunkCodec::Deflate;
        directory.entries[0].storedByteLength = compressed.size();
        directory.entries[1].offset = compressed.size();
        directory.entries[2].offset = compressed.size() + directory.entries[1].storedByteLength;
        auto undersized = directory;
        undersized.entries[0].decodedByteLength = 2;
        const auto undersizedProof = ValidateSaveChunkDirectory(std::move(undersized), Manifest());
        REQUIRE(undersizedProof.HasValue());
        const auto rejected = SelectSaveChunkPayload(payload, undersizedProof.Value(), Id<SaveRecordId>(20));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == SaveErrors::ArchiveChunkDecodeFailed.code.Value());

        const auto validated = ValidateSaveChunkDirectory(std::move(directory), Manifest());
        REQUIRE(validated.HasValue());
        const auto selected = SelectSaveChunkPayload(payload, validated.Value(), Id<SaveRecordId>(20));
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value());
        const std::vector<std::byte> expected{raw.begin(), raw.begin() + 3};
        CHECK(*selected.Value() == expected);
    }

    TEST_CASE("Compressed chunks reject trailing bytes inside their declared stored range", "[runtime][save][compression]") {
        std::vector<std::byte> canonical(2'048);
        for (std::size_t index = 0; index < canonical.size(); ++index)
            canonical[index] = static_cast<std::byte>(index % 64);
        const auto encoded = EncodeSaveChunk(canonical, {.preferred = SaveChunkCodec::Deflate, .required = true}, {});
        REQUIRE(encoded.HasValue());
        auto payload = encoded.Value().stored;
        payload.push_back(std::byte{0x7f});
        const auto raw = Payload();
        payload.insert(payload.end(), raw.begin() + 3, raw.end());
        auto directory = Directory(raw);
        directory.payloadByteLength = payload.size();
        directory.entries[0].codec = SaveChunkCodec::Deflate;
        directory.entries[0].storedByteLength = encoded.Value().stored.size() + 1;
        directory.entries[0].decodedByteLength = canonical.size();
        directory.entries[0].decodedHash = encoded.Value().decodedHash;
        directory.entries[1].offset = directory.entries[0].storedByteLength;
        directory.entries[2].offset = directory.entries[1].offset + directory.entries[1].storedByteLength;
        const auto validated = ValidateSaveChunkDirectory(std::move(directory), Manifest());
        REQUIRE(validated.HasValue());
        const auto selected = SelectSaveChunkPayload(payload, validated.Value(), Id<SaveRecordId>(20));
        REQUIRE(selected.HasError());
        CHECK(selected.ErrorValue().code.Value() == SaveErrors::ArchiveChunkDecodeFailed.code.Value());
    }

    TEST_CASE("Save chunk directory rejects gaps overlap truncation and trailing bytes", "[runtime][save][framing]") {
        const auto payload = Payload();
        auto directory = Directory(payload);
        directory.entries[1].offset = 2;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.entries[1].offset = 4;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.payloadByteLength = 8;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());

        directory = Directory(payload);
        const auto validated = ValidateSaveChunkDirectory(std::move(directory), Manifest()).Value();
        CHECK(SelectSaveChunkPayload(std::span<const std::byte>{payload}.first(8), validated, Id<SaveRecordId>(20)).HasError());
        auto trailing = payload;
        trailing.push_back(std::byte{});
        CHECK(SelectSaveChunkPayload(trailing, validated, Id<SaveRecordId>(20)).HasError());
    }

    TEST_CASE("Save chunk directory rejects duplicate ordering ownership and manifest mismatch", "[runtime][save][framing]") {
        const auto payload = Payload();
        auto directory = Directory(payload);
        directory.entries[1].record = directory.entries[0].record;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.entries[1].owner = directory.entries[2].owner;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.entries.pop_back();
        directory.payloadByteLength = 5;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
    }

    TEST_CASE("Save chunk directory rejects unsafe scalar fields and explicit limit violations", "[runtime][save][framing]") {
        const auto payload = Payload();
        auto directory = Directory(payload);
        directory.entries[0].alignment = 3;
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.entries[0].storedByteLength = std::numeric_limits<std::uint64_t>::max();
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());
        directory = Directory(payload);
        directory.entries[0].codec = static_cast<SaveChunkCodec>(99);
        CHECK(ValidateSaveChunkDirectory(directory, Manifest()).HasError());

        auto limits = SaveChunkDirectoryLimits{};
        limits.maximumEntries = 2;
        CHECK(ValidateSaveChunkDirectory(Directory(payload), Manifest(), limits).HasError());
        limits = {};
        limits.maximumDecodedChunkBytes = 2;
        CHECK(ValidateSaveChunkDirectory(Directory(payload), Manifest(), limits).HasError());
    }

    TEST_CASE("Save integrity verifies canonical state, whole archive coverage, and every entry", "[runtime][save][integrity]") {
        const auto payload = Payload();
        const auto preamble = std::vector<std::byte>(SaveArchivePreambleByteLength, std::byte{7});
        const auto validated = ValidateSaveChunkDirectory(Directory(payload), Manifest()).Value();
        const auto finalized = FinalizeSaveArchiveIntegrity(preamble, payload);
        REQUIRE(finalized.HasValue());
        CHECK(finalized.Value().algorithm == SaveIntegrityAlgorithmVersion{});
        CHECK(finalized.Value().preambleByteLength == SaveArchivePreambleByteLength);
        CHECK(finalized.Value().payloadByteLength == payload.size());
        CHECK(finalized.Value().trailerByteLength == SaveArchiveUnsignedTrailerByteLength);

        std::vector<std::byte> archive = preamble;
        archive.insert(archive.end(), payload.begin(), payload.end());
        archive.insert(archive.end(), SaveArchiveUnsignedTrailerByteLength, std::byte{});
        CHECK(VerifySaveArchiveIntegrity(finalized.Value(), archive, validated).HasValue());

        auto modifiedPreamble = archive;
        modifiedPreamble[0] = std::byte{8};
        CHECK(VerifySaveArchiveIntegrity(finalized.Value(), modifiedPreamble, validated).ErrorValue().code.Value() ==
              SaveErrors::ArchiveContentHashMismatch.code.Value());

        auto truncated = archive;
        truncated.pop_back();
        CHECK(VerifySaveArchiveIntegrity(finalized.Value(), truncated, validated).ErrorValue().code.Value() ==
              SaveErrors::ArchivePayloadTruncated.code.Value());

        auto substituted = archive;
        substituted[SaveArchivePreambleByteLength + 3] = std::byte{99};
        auto updatedIntegrity = finalized.Value();
        updatedIntegrity.archiveContent =
            ComputeArchiveContentHash(std::span<const std::byte>{substituted}.first(SaveArchivePreambleByteLength),
                                      std::span<const std::byte>{substituted}.subspan(SaveArchivePreambleByteLength, payload.size()));
        REQUIRE(VerifySaveArchiveIntegrity(updatedIntegrity, substituted, validated).HasValue());
        const auto entryFailure =
            SelectSaveChunkPayload(std::span<const std::byte>{substituted}.subspan(SaveArchivePreambleByteLength, payload.size()),
                                   validated, Id<SaveRecordId>(21));
        REQUIRE(entryFailure.HasError());
        CHECK(entryFailure.ErrorValue().code.Value() == SaveErrors::ArchiveChunkHashMismatch.code.Value());
        REQUIRE(!entryFailure.ErrorValue().diagnostics.empty());
        CHECK(entryFailure.ErrorValue().diagnostics.front().path ==
              "participant/" + validated.Entries()[1].owner.Value() + "/record/" + validated.Entries()[1].record.ToString());

        const std::array canonicalFirst{std::byte{1}, std::byte{2}};
        const std::array canonicalSecond{std::byte{3}, std::byte{4}};
        const std::array fragments{std::span<const std::byte>{canonicalFirst}, std::span<const std::byte>{canonicalSecond}};
        std::vector<std::byte> canonicalConcatenated{canonicalFirst.begin(), canonicalFirst.end()};
        canonicalConcatenated.insert(canonicalConcatenated.end(), canonicalSecond.begin(), canonicalSecond.end());
        CHECK(ComputeSha256Fragments(fragments) == ComputeSha256(canonicalConcatenated));
        const auto canonicalHash = ComputeCanonicalStateHash(canonicalConcatenated);
        CHECK(VerifyCanonicalStateHash(canonicalHash, canonicalConcatenated).HasValue());
        canonicalConcatenated.back() = std::byte{5};
        CHECK(VerifyCanonicalStateHash(canonicalHash, canonicalConcatenated).ErrorValue().code.Value() ==
              SaveErrors::CanonicalStateHashMismatch.code.Value());
    }

    TEST_CASE("Save integrity uses one explicit domain terminator", "[runtime][save][integrity]") {
        const std::array canonicalState{std::byte{1}, std::byte{2}, std::byte{3}};
        constexpr std::string_view tag = "HoroSave.CanonicalState.v1";
        const auto tagBytes = std::as_bytes(std::span<const char>{tag.data(), tag.size()});
        const std::array<std::byte, 1> terminator{};
        const std::array fragments{tagBytes, std::span<const std::byte>{terminator}, std::span<const std::byte>{canonicalState}};

        CHECK(ComputeCanonicalStateHash(canonicalState).value == ComputeSha256Fragments(fragments));
    }

    TEST_CASE("Save integrity rejects unsupported algorithms and ambiguous coverage", "[runtime][save][integrity]") {
        const auto payload = Payload();
        const auto preamble = std::vector<std::byte>(SaveArchivePreambleByteLength, std::byte{});
        CHECK(FinalizeSaveArchiveIntegrity(preamble, payload, SaveArchiveUnsignedTrailerByteLength,
                                           {.algorithm = static_cast<SaveIntegrityHashAlgorithm>(99), .version = 1})
                  .ErrorValue()
                  .code.Value() == SaveErrors::ArchiveIntegrityAlgorithmUnsupported.code.Value());
        CHECK(FinalizeSaveArchiveIntegrity(preamble, payload, SaveArchiveUnsignedTrailerByteLength,
                                           {.algorithm = SaveIntegrityHashAlgorithm::Sha256, .version = 2})
                  .ErrorValue()
                  .code.Value() == SaveErrors::ArchiveIntegrityAlgorithmUnsupported.code.Value());
        CHECK(FinalizeSaveArchiveIntegrity(preamble, payload, 1).ErrorValue().code.Value() ==
              SaveErrors::ArchiveIntegrityCoverageInvalid.code.Value());
        CHECK(FinalizeSaveArchiveIntegrity(std::span<const std::byte>{preamble}.first(31), payload).ErrorValue().code.Value() ==
              SaveErrors::ArchiveIntegrityCoverageInvalid.code.Value());
    }
}  // namespace
