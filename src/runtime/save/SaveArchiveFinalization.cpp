#include "Horo/Runtime/Save/SaveArchiveFinalization.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <limits>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Error InvalidFinalization(std::string message) {
            return MakeError(SaveErrors::ArchiveDirectoryInvalid, std::move(message));
        }

        [[nodiscard]] Error CleanupFailure(Error cause) {
            return WrapError(SaveErrors::StoragePermanentIo, std::move(cause), "Finalized save temporary cleanup failed.");
        }

        /** @brief Writes the v1 archive-content digest into the fixed integrity trailer. */
        void SerializeIntegrityTrailer(std::vector<std::byte> &archive, const SaveArchiveIntegrityManifest &integrity) {
            const auto trailerOffset = static_cast<std::size_t>(integrity.preambleByteLength + integrity.payloadByteLength);
            for (std::size_t index = 0; index < integrity.archiveContent.value.bytes.size(); ++index)
                archive[trailerOffset + index] = static_cast<std::byte>(integrity.archiveContent.value.bytes[index]);
        }
    }  // namespace

    /** @copydoc FinalizedSaveArchive::Archive */
    ImmutableSaveArchive FinalizedSaveArchive::Archive() const noexcept {
        return {.bytes = bytes_};
    }

    /** @copydoc FinalizedSaveArchive::Summary */
    const SaveArchiveFinalizationSummary &FinalizedSaveArchive::Summary() const noexcept {
        return summary_;
    }

    /** @copydoc FinalizedSaveArchive::Bytes */
    std::span<const std::byte> FinalizedSaveArchive::Bytes() const noexcept {
        return bytes_ == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>{*bytes_};
    }

    FinalizedSaveArchive::FinalizedSaveArchive(std::shared_ptr<const std::vector<std::byte>> bytes,
                                               SaveArchiveFinalizationSummary summary) noexcept
        : bytes_(std::move(bytes)), summary_(std::move(summary)) {}

    /** @copydoc SaveArchiveFinalizer::Create */
    Result<SaveArchiveFinalizer> SaveArchiveFinalizer::Create(std::vector<std::byte> preamble, SaveGameManifest manifest,
                                                              SaveChunkDirectory directory, const SaveArchiveFinalizationLimits &limits) {
        if (preamble.size() != SaveArchivePreambleByteLength || limits.maximumArchiveBytes == 0 ||
            limits.maximumArchiveBytes < SaveArchivePreambleByteLength + SaveArchiveUnsignedTrailerByteLength)
            return Result<SaveArchiveFinalizer>::Failure(MakeError(SaveErrors::ArchiveIntegrityCoverageInvalid));
        if (const auto valid = ValidateSaveGameManifest(manifest); valid.HasError())
            return Result<SaveArchiveFinalizer>::Failure(valid.ErrorValue());
        auto validatedDirectory = ValidateSaveChunkDirectory(std::move(directory), manifest, limits.directory);
        if (validatedDirectory.HasError())
            return Result<SaveArchiveFinalizer>::Failure(validatedDirectory.ErrorValue());
        if (validatedDirectory.Value().PayloadByteLength() >
            limits.maximumArchiveBytes - SaveArchivePreambleByteLength - SaveArchiveUnsignedTrailerByteLength)
            return Result<SaveArchiveFinalizer>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        try {
            return Result<SaveArchiveFinalizer>::Success(
                SaveArchiveFinalizer{std::move(preamble), std::move(manifest), std::move(validatedDirectory).Value(), limits});
        } catch (const std::bad_alloc &) {
            return Result<SaveArchiveFinalizer>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    SaveArchiveFinalizer::SaveArchiveFinalizer(std::vector<std::byte> preamble, SaveGameManifest manifest,
                                               ValidatedSaveChunkDirectory directory, const SaveArchiveFinalizationLimits &limits) noexcept
        : preamble_(std::move(preamble)), manifest_(std::move(manifest)), directory_(std::move(directory)), limits_(limits) {}

    /** @copydoc SaveArchiveFinalizer::AppendChunk */
    Result<void> SaveArchiveFinalizer::AppendChunk(const SaveRecordId record, const std::span<const std::byte> bytes) {
        if (finalized_)
            return Result<void>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));
        const auto entries = directory_.Entries();
        if (nextEntry_ >= entries.size() || entries[nextEntry_].record != record)
            return Result<void>::Failure(InvalidFinalization("Save archive chunks must be appended in validated record order."));
        const SaveChunkDirectoryEntry &entry = entries[nextEntry_];
        if (bytes.size() != entry.storedByteLength)
            return Result<void>::Failure(MakeError(SaveErrors::ArchivePayloadTruncated));
        if (entry.codec != SaveChunkCodec::Raw || ComputeSha256(bytes) != entry.decodedHash)
            return Result<void>::Failure(MakeError(SaveErrors::ArchiveChunkHashMismatch));
        if (payload_.size() > std::numeric_limits<std::size_t>::max() - bytes.size())
            return Result<void>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        const auto nextPayloadByteLength = static_cast<std::uint64_t>(payload_.size() + bytes.size());
        const auto maximumArchivePayloadBytes =
            limits_.maximumArchiveBytes - SaveArchivePreambleByteLength - SaveArchiveUnsignedTrailerByteLength;
        if (nextPayloadByteLength > limits_.directory.maximumPayloadBytes || nextPayloadByteLength > maximumArchivePayloadBytes)
            return Result<void>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        try {
            payload_.insert(payload_.end(), bytes.begin(), bytes.end());
            ++nextEntry_;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    bool SaveArchiveFinalizer::FitsArchive(const std::uint64_t trailerByteLength) const noexcept {
        return preamble_.size() <= limits_.maximumArchiveBytes && payload_.size() <= limits_.maximumArchiveBytes - preamble_.size() &&
               trailerByteLength <= limits_.maximumArchiveBytes - preamble_.size() - payload_.size();
    }

    /** @copydoc SaveArchiveFinalizer::Finalize */
    Result<FinalizedSaveArchive> SaveArchiveFinalizer::Finalize(const std::uint32_t trailerByteLength) {
        if (finalized_)
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CaptureAlreadySealed));
        if (nextEntry_ != directory_.Entries().size() || payload_.size() != directory_.PayloadByteLength())
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CaptureIncomplete));
        if (!FitsArchive(trailerByteLength))
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));

        const auto integrity = FinalizeSaveArchiveIntegrity(preamble_, payload_, trailerByteLength);
        if (integrity.HasError())
            return Result<FinalizedSaveArchive>::Failure(integrity.ErrorValue());
        try {
            auto archive = std::make_shared<std::vector<std::byte>>();
            archive->reserve(preamble_.size() + payload_.size() + trailerByteLength);
            archive->insert(archive->end(), preamble_.begin(), preamble_.end());
            archive->insert(archive->end(), payload_.begin(), payload_.end());
            archive->insert(archive->end(), trailerByteLength, std::byte{});
            SerializeIntegrityTrailer(*archive, integrity.Value());
            if (const auto verified = VerifySaveArchiveIntegrity(integrity.Value(), *archive, directory_); verified.HasError())
                return Result<FinalizedSaveArchive>::Failure(verified.ErrorValue());
            finalized_ = true;
            const SaveArchiveFinalizationSummary summary{.integrity = integrity.Value(),
                                                         .canonicalState = manifest_.canonicalState,
                                                         .archiveByteLength = archive->size(),
                                                         .payloadByteLength = payload_.size(),
                                                         .entryCount = directory_.Entries().size()};
            return Result<FinalizedSaveArchive>::Success(FinalizedSaveArchive{std::move(archive), summary});
        } catch (const std::bad_alloc &) {
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }

    /** @copydoc SaveArchiveFinalizer::FinalizeTo */
    Result<FinalizedSaveArchive> SaveArchiveFinalizer::FinalizeTo(DurableFileSystem &files, const std::filesystem::path &temporaryPath,
                                                                  const std::uint32_t trailerByteLength) {
        if (temporaryPath.empty() || temporaryPath.filename().empty())
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        auto finalized = Finalize(trailerByteLength);
        if (finalized.HasError())
            return finalized;
        if (const auto written = files.WriteDurable(temporaryPath, finalized.Value().Bytes()); written.HasError()) {
            static_cast<void>(files.RemoveDurable(temporaryPath));
            return Result<FinalizedSaveArchive>::Failure(written.ErrorValue());
        }
        temporaryPath_ = temporaryPath;
        return finalized;
    }

    /** @copydoc SaveArchiveFinalizer::DiscardTemporary */
    Result<void> SaveArchiveFinalizer::DiscardTemporary(DurableFileSystem &files) {
        if (!temporaryPath_)
            return Result<void>::Success();
        if (const auto removed = files.RemoveDurable(*temporaryPath_); removed.HasError())
            return Result<void>::Failure(CleanupFailure(removed.ErrorValue()));
        temporaryPath_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
