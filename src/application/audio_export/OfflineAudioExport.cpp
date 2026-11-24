#include "Horo/Application/Internal/OfflineAudioExport.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "WaveEncoding.h"

#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Outcome = Result<OfflineAudioWaveExportSummary>;

        /** @brief Admit only the reserved staging child and existing no-follow directories through the publication authority. */
        Result<void> AdmitPaths(const OfflineAudioWaveExportRequest &request, const DurableFileSystem &files) {
            if (!request.preparedFile.is_absolute() || !request.destination.is_absolute() || !request.preparedFile.has_filename() ||
                !request.destination.has_filename() || request.preparedFile != request.preparedFile.lexically_normal() ||
                request.destination != request.destination.lexically_normal() ||
                request.preparedFile.parent_path().parent_path() != request.destination.parent_path() ||
                request.preparedFile.parent_path().filename() != OfflineAudioWaveStagingDirectoryName)
                return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
            const auto staging = files.InspectEntry(request.preparedFile.parent_path());
            if (staging.HasError())
                return Result<void>::Failure(staging.ErrorValue());
            const auto destination = files.InspectEntry(request.destination.parent_path());
            if (destination.HasError())
                return Result<void>::Failure(destination.ErrorValue());
            if (!staging.Value() || !destination.Value())
                return Result<void>::Failure(MakeError(FileStorageErrors::InvalidPath));
            if (staging.Value()->kind != FileEntryKind::Directory || destination.Value()->kind != FileEntryKind::Directory)
                return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            return Result<void>::Success();
        }

        /** @brief Produces file shape only if content and tail fit without overflowing the frame count. */
        std::optional<WaveShape> ExportShape(const OfflineAudioWaveExportRequest &request) noexcept {
            if (request.render.contentFrames > std::numeric_limits<std::uint64_t>::max() - request.render.tailFrames)
                return std::nullopt;
            return PrepareWaveShape(request.render.format, request.render.contentFrames + request.render.tailFrames,
                                    request.maximumEncodedBytes);
        }

        /** @brief Acquires the staging namespace's existing export lease and verifies its scope. */
        Result<ExclusiveFileLock> ClaimWriter(const OfflineAudioWaveExportRequest &request, DurableFileSystem &files) {
            const auto lockPath = request.preparedFile.parent_path() / OfflineAudioWaveWriterLock;
            auto acquired = files.TryAcquireExclusive(lockPath, "horo.audio.export");
            if (acquired.HasValue() && !acquired.Value().ProtectsPath(lockPath))
                return Result<ExclusiveFileLock>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            return acquired;
        }

        /** @brief Admits exactly one export per receipt, including attempts that created no file. */
        bool FreshReceipt(const OfflineAudioWaveExportReceipt &receipt) noexcept {
            return !receipt.admitted && !receipt.preparedCreated && !receipt.preparedRemoved && !receipt.publication.WasCommitted();
        }

        /** @brief Cleanup only invocation-created pre-commit storage, preserving both original and cleanup failure evidence. */
        Outcome Fail(Error error, const OfflineAudioWaveExportRequest &request, DurableFileSystem &files,
                     OfflineAudioWaveExportReceipt &receipt) {
            if (receipt.preparedCreated && !receipt.preparedRemoved && !receipt.publication.WasCommitted()) {
                const auto removed = files.RemoveDurable(request.preparedFile);
                if (removed.HasError())
                    error = WithCause(removed.ErrorValue(), std::move(error));
                else
                    receipt.preparedRemoved = true;
            }
            return Outcome::Failure(std::move(error));
        }

        /** @brief Prepares and commits encoded bytes, reporting the exact phase in the existing receipt. */
        Result<void> PublishWave(const OfflineAudioWaveExportRequest &request, std::span<const std::byte> bytes, DurableFileSystem &files,
                                 OfflineAudioWaveExportReceipt &receipt) {
            if (request.render.cancellation.stop_requested())
                return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            const auto available = files.AvailableBytes(request.preparedFile.parent_path());
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (available.Value() < bytes.size())
                return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
            if (const auto written = files.WritePrivateDurable(request.preparedFile, bytes, receipt.preparedCreated); written.HasError())
                return written;
            if (!receipt.preparedCreated)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Private export creation was not confirmed"));
            if (request.render.cancellation.stop_requested())
                return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            if (const auto published = files.AtomicReplaceTracked(request.preparedFile, request.destination, receipt.publication);
                published.HasError())
                return published;
            if (!receipt.publication.WasCommitted())
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed, "Atomic export commit was not confirmed"));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExportOfflineAudioWave */
    Result<OfflineAudioWaveExportSummary> ExportOfflineAudioWave(const OfflineAudioWaveExportRequest &request,
                                                                 const Audio::Backend::RenderPort render, DurableFileSystem &files,
                                                                 OfflineAudioWaveExportReceipt &receipt) {
        if (!FreshReceipt(receipt))
            return Outcome::Failure(MakeError(FileStorageErrors::Conflict));
        std::optional<ExclusiveFileLock> writer;  // Retained through catch-side cleanup, not unwound before it.
        try {
            if (request.render.cancellation.stop_requested())
                return Outcome::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            const auto shape = ExportShape(request);
            if (!shape)
                return Outcome::Failure(MakeError(Audio::AudioErrors::FormatCombinationUnsupported));
            if (const auto paths = AdmitPaths(request, files); paths.HasError())
                return Outcome::Failure(paths.ErrorValue());
            auto acquired = ClaimWriter(request, files);
            if (acquired.HasError())
                return Outcome::Failure(acquired.ErrorValue());
            writer.emplace(std::move(acquired).Value());
            receipt.admitted = true;
            auto pcm = Audio::Backend::RenderOfflineAudio(request.render, render);
            if (pcm.HasError())
                return Outcome::Failure(pcm.ErrorValue());
            auto bytes =
                EncodeWave(*shape, pcm.Value().format.sampleRate, pcm.Value().interleaved, {.offline = request.render.cancellation});
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            if (const auto published = PublishWave(request, bytes.Value(), files, receipt); published.HasError())
                return Fail(published.ErrorValue(), request, files, receipt);
            return Outcome::Success(
                {pcm.Value().contentFrames, pcm.Value().tailFrames, shape->encodedBytes, pcm.Value().format.sampleRate, shape->mask});
        } catch (const std::bad_alloc &) {
            return Fail(MakeError(Audio::AudioErrors::MemoryAllocationFailed), request, files, receipt);
        }
    }

    /** @copydoc ExportOfflineAudioWave */
    Result<OfflineAudioWaveExportSummary> ExportOfflineAudioWave(const OfflineAudioWaveExportRequest &request,
                                                                 const Audio::Backend::OfflineMixerSource &source, DurableFileSystem &files,
                                                                 OfflineAudioWaveExportReceipt &receipt) {
        auto prepared = Audio::Backend::OfflineMixerSession::Prepare(request.render, source);
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto session = std::move(prepared).Value();
        return ExportOfflineAudioWave(request, session.Port(), files, receipt);
    }
}  // namespace Horo::Application::Internal
