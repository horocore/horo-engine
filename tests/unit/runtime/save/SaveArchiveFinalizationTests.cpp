#include "Horo/Runtime/Save/SaveArchiveFinalization.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Test;

    struct DurableFileSystemStub final : DurableFileSystem {
        std::unordered_map<std::string, std::vector<std::byte>> files;
        bool failWrite{};
        bool failRemove{};
        const ErrorCodeDescriptor *writeFailure{&SaveErrors::StoragePermanentIo};
        const ErrorCodeDescriptor *removeFailure{&SaveErrors::StoragePermanentIo};

        Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &, std::string_view) override {
            return Result<ExclusiveFileLock>::Failure(MakeError(SaveErrors::StoragePermanentIo));
        }

        Result<std::uint64_t> AvailableBytes(const std::filesystem::path &) const override {
            return Result<std::uint64_t>::Success(1024);
        }

        Result<void> WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) override {
            if (failWrite)
                return Result<void>::Failure(MakeError(*writeFailure));
            files[path.string()] = {bytes.begin(), bytes.end()};
            return Result<void>::Success();
        }

        Result<void> CopyDurable(const std::filesystem::path &, const std::filesystem::path &) override {
            return Result<void>::Failure(MakeError(SaveErrors::StoragePermanentIo));
        }

        Result<void> AtomicReplace(const std::filesystem::path &, const std::filesystem::path &) override {
            return Result<void>::Failure(MakeError(SaveErrors::StoragePermanentIo));
        }

        Result<void> RemoveDurable(const std::filesystem::path &path) override {
            if (failRemove)
                return Result<void>::Failure(MakeError(*removeFailure));
            files.erase(path.string());
            return Result<void>::Success();
        }

        Result<void> SyncDirectory(const std::filesystem::path &) override {
            return Result<void>::Success();
        }
    };

    struct Fixture final {
        std::vector<std::byte> first{std::byte{1}, std::byte{2}, std::byte{3}};
        std::vector<std::byte> second{std::byte{4}, std::byte{5}};
        SaveGameManifest manifest{.saveSchemaVersion = V<SaveSchemaVersion>(1),
                                  .canonicalState = {.value = Digest(99)},
                                  .participants = StandardParticipants(1, 1)};
        std::vector<std::byte> preamble{SaveArchivePreambleByteLength, std::byte{7}};

        SaveChunkDirectory Directory() const {
            return {.integrityAlgorithm = {},
                    .payloadByteLength = first.size() + second.size() + 1,
                    .entries = {{.record = Id<SaveRecordId>(20),
                                 .owner = manifest.participants[0].participant,
                                 .offset = 0,
                                 .storedByteLength = first.size(),
                                 .decodedByteLength = first.size(),
                                 .alignment = 1,
                                 .codec = SaveChunkCodec::Raw,
                                 .decodedHash = ComputeSha256(first)},
                                {.record = Id<SaveRecordId>(21),
                                 .owner = manifest.participants[0].participant,
                                 .offset = first.size(),
                                 .storedByteLength = second.size(),
                                 .decodedByteLength = second.size(),
                                 .alignment = 1,
                                 .codec = SaveChunkCodec::Raw,
                                 .decodedHash = ComputeSha256(second)},
                                {.record = Id<SaveRecordId>(22),
                                 .owner = manifest.participants[1].participant,
                                 .offset = first.size() + second.size(),
                                 .storedByteLength = 1,
                                 .decodedByteLength = 1,
                                 .alignment = 1,
                                 .codec = SaveChunkCodec::Raw,
                                 .decodedHash = ComputeSha256(std::array<std::byte, 1>{std::byte{6}})}}};
        }
    };

    TEST_CASE("Save archive finalization admits only complete ordered chunks", "[runtime][save][finalization]") {
        Fixture fixture;
        auto created = SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory());
        REQUIRE(created.HasValue());
        auto finalizer = std::move(created).Value();
        CHECK(finalizer.AppendChunk(Id<SaveRecordId>(21), fixture.second).HasError());
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(20), fixture.first).HasValue());
        CHECK(finalizer.Finalize().ErrorValue().code.Value() == SaveErrors::CaptureIncomplete.code.Value());
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(21), fixture.second).HasValue());
        const std::array last{std::byte{6}};
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(22), last).HasValue());

        auto archive = finalizer.Finalize();
        REQUIRE(archive.HasValue());
        CHECK(archive.Value().Summary().canonicalState == fixture.manifest.canonicalState);
        CHECK(archive.Value().Summary().entryCount == 3);
        CHECK(archive.Value().Bytes().size() == SaveArchivePreambleByteLength + 6 + SaveArchiveUnsignedTrailerByteLength);
        const auto trailer = archive.Value().Bytes().subspan(SaveArchivePreambleByteLength + 6);
        CHECK(std::equal(archive.Value().Summary().integrity.archiveContent.value.bytes.begin(),
                         archive.Value().Summary().integrity.archiveContent.value.bytes.end(), trailer.begin(),
                         [](const std::uint8_t actual, const std::byte stored) {
            return static_cast<std::byte>(actual) == stored;
        }));
        CHECK(std::all_of(trailer.begin() + archive.Value().Summary().integrity.archiveContent.value.bytes.size(), trailer.end(),
                          [](const std::byte byte) {
            return byte == std::byte{};
        }));
        CHECK(archive.Value().Archive().bytes != nullptr);
        CHECK(finalizer.Finalize().ErrorValue().code.Value() == SaveErrors::CaptureAlreadySealed.code.Value());
    }

    TEST_CASE("Save archive finalization rejects chunk hash mismatches", "[runtime][save][finalization]") {
        Fixture fixture;
        auto finalizer = std::move(SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory())).Value();
        const std::array tooShort{std::byte{9}, std::byte{9}};
        CHECK(finalizer.AppendChunk(Id<SaveRecordId>(20), tooShort).ErrorValue().code.Value() ==
              SaveErrors::ArchiveChunkHashMismatch.code.Value());
        const std::array wrong{std::byte{9}, std::byte{9}, std::byte{9}};
        CHECK(finalizer.AppendChunk(Id<SaveRecordId>(20), wrong).ErrorValue().code.Value() ==
              SaveErrors::ArchiveChunkHashMismatch.code.Value());
        CHECK(finalizer.AppendChunk(Id<SaveRecordId>(20), fixture.first).HasValue());
    }

    TEST_CASE("Save archive finalization enforces the archive size budget before staging", "[runtime][save][finalization]") {
        Fixture fixture;
        SaveArchiveFinalizationLimits limits;
        limits.maximumArchiveBytes = SaveArchivePreambleByteLength + 6 + SaveArchiveUnsignedTrailerByteLength - 1;
        CHECK(SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory(), limits).ErrorValue().code.Value() ==
              SaveErrors::ArchiveFramingLimitExceeded.code.Value());
    }

    TEST_CASE("Save archive finalization enforces the payload limit before staging", "[runtime][save][finalization]") {
        Fixture fixture;
        SaveArchiveFinalizationLimits limits;
        limits.directory.maximumPayloadBytes = fixture.first.size() + fixture.second.size();
        CHECK(SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory(), limits).ErrorValue().code.Value() ==
              SaveErrors::ArchiveFramingLimitExceeded.code.Value());
    }

    TEST_CASE("Save archive finalization stages chunks at the exact archive and payload limits", "[runtime][save][finalization]") {
        Fixture fixture;
        SaveArchiveFinalizationLimits limits;
        limits.directory.maximumPayloadBytes = fixture.Directory().payloadByteLength;
        limits.maximumArchiveBytes = SaveArchivePreambleByteLength + fixture.Directory().payloadByteLength + SaveArchiveUnsignedTrailerByteLength;
        auto created = SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory(), limits);
        REQUIRE(created.HasValue());
        auto finalizer = std::move(created).Value();

        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(20), fixture.first).HasValue());
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(21), fixture.second).HasValue());
        const std::array last{std::byte{6}};
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(22), last).HasValue());
        const auto finalized = finalizer.Finalize();
        REQUIRE(finalized.HasValue());
        CHECK(finalized.Value().Bytes().size() == limits.maximumArchiveBytes);
    }

    TEST_CASE("Save archive finalization writes and removes only its operation temporary", "[runtime][save][finalization]") {
        Fixture fixture;
        auto finalizer = std::move(SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory())).Value();
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(20), fixture.first).HasValue());
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(21), fixture.second).HasValue());
        const std::array last{std::byte{6}};
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(22), last).HasValue());

        DurableFileSystemStub files;
        const auto temporary = std::filesystem::path{"save-operation.tmp"};
        REQUIRE(finalizer.FinalizeTo(files, temporary).HasValue());
        REQUIRE(files.files.contains(temporary.string()));
        REQUIRE(finalizer.DiscardTemporary(files).HasValue());
        CHECK_FALSE(files.files.contains(temporary.string()));
    }

    TEST_CASE("Save archive finalization removes a failed write when possible", "[runtime][save][finalization]") {
        Fixture fixture;
        auto finalizer = std::move(SaveArchiveFinalizer::Create(fixture.preamble, fixture.manifest, fixture.Directory())).Value();
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(20), fixture.first).HasValue());
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(21), fixture.second).HasValue());
        const std::array last{std::byte{6}};
        REQUIRE(finalizer.AppendChunk(Id<SaveRecordId>(22), last).HasValue());

        DurableFileSystemStub files;
        files.failWrite = true;
        files.failRemove = true;
        files.writeFailure = &SaveErrors::StorageDiskFull;
        files.removeFailure = &SaveErrors::StoragePermanentIo;
        const auto result = finalizer.FinalizeTo(files, "save-operation.tmp");
        CHECK(result.HasError());
        CHECK(result.ErrorValue().code.Value() == SaveErrors::StorageDiskFull.code.Value());
        CHECK_FALSE(files.files.contains("save-operation.tmp"));
    }
}  // namespace
