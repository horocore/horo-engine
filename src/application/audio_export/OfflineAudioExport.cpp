#include "Horo/Application/Internal/OfflineAudioExport.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Foundation/ImmutableFileStorage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Outcome = Result<OfflineAudioWaveExportSummary>;
        constexpr std::uint64_t WaveHeaderBytes = 80;
        static_assert(sizeof(Audio::AudioSample) == 4 && std::numeric_limits<Audio::AudioSample>::is_iec559);

        /** @brief WAVE interchange bit positions are explicit; Horo enum integers are never serialized as native masks. */
        std::uint32_t SpeakerBit(const Audio::AudioSpeakerRole role) noexcept {
            using enum Audio::AudioSpeakerRole;
            switch (role) {
                case FrontLeft:
                    return 0x1;
                case FrontRight:
                    return 0x2;
                case FrontCenter:
                    return 0x4;
                case LowFrequency:
                    return 0x8;
                case BackLeft:
                    return 0x10;
                case BackRight:
                    return 0x20;
                case SideLeft:
                    return 0x200;
                case SideRight:
                    return 0x400;
                case TopFrontLeft:
                    return 0x1000;
                case TopFrontRight:
                    return 0x4000;
                case TopBackLeft:
                    return 0x8000;
                case TopBackRight:
                    return 0x20000;
            }
            return 0;
        }

        /** @brief Control-owned fixed storage describing exact speaker-preserving file order. */
        struct WaveShape final {
            std::array<std::pair<std::uint32_t, std::size_t>, Audio::MaximumAudioChannels> channels{};
            std::uint32_t count{}, mask{};
            std::uint64_t frames{}, encodedBytes{};
        };

        /** @brief Admit the complete encoding size and semantic permutation before consuming any prepared core state. */
        std::optional<WaveShape> Shape(const OfflineAudioWaveExportRequest &request) noexcept {
            const auto &format = request.render.format;
            if (!Audio::ValidateAudioProcessingFormat(format) || format.layout.kind != Audio::AudioLayoutKind::Speaker ||
                request.render.contentFrames > std::numeric_limits<std::uint64_t>::max() - request.render.tailFrames)
                return std::nullopt;
            WaveShape shape;
            shape.count = static_cast<std::uint32_t>(format.layout.orderedChannels.size());
            shape.frames = request.render.contentFrames + request.render.tailFrames;
            const auto bytesPerFrame = shape.count * sizeof(Audio::AudioSample);
            if (request.maximumEncodedBytes > Audio::Backend::MaximumOfflineAudioOutputBytes ||
                request.maximumEncodedBytes < WaveHeaderBytes || shape.frames == 0 ||
                shape.frames > (request.maximumEncodedBytes - WaveHeaderBytes) / bytesPerFrame)
                return std::nullopt;
            shape.encodedBytes = WaveHeaderBytes + shape.frames * bytesPerFrame;
            for (std::size_t index = 0; index < shape.count; ++index) {
                const auto *role = std::get_if<Audio::AudioSpeakerRole>(&format.layout.orderedChannels[index]);
                if (!role)
                    return std::nullopt;
                const auto bit = SpeakerBit(*role);
                if (!bit || (shape.mask & bit))
                    return std::nullopt;
                shape.mask |= bit;
                shape.channels[index] = {bit, index};
            }
            std::sort(shape.channels.begin(), shape.channels.begin() + shape.count);
            return shape;
        }

        /** @brief Admit only the reserved staging child and existing no-follow directories through the publication authority. */
        Result<void> AdmitPaths(const OfflineAudioWaveExportRequest &request, DurableFileSystem &files) {
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

        /** @brief Write one unsigned little-endian integer into already sized file storage. */
        void Integer(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint32_t value,
                     const std::size_t width = 4) noexcept {
            for (std::size_t index = 0; index < width; ++index)
                bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xff);
        }

        /** @brief Write a fixed-size RIFF chunk identifier without a terminator. */
        void Four(std::vector<std::byte> &bytes, const std::size_t offset, const char (&text)[5]) noexcept {
            for (std::size_t index = 0; index < 4; ++index)
                bytes[offset + index] = static_cast<std::byte>(text[index]);
        }

        /** @brief Encode the standard extensible IEEE-float header, including the non-PCM fact frame count. */
        void Header(std::vector<std::byte> &bytes, const WaveShape &shape, const std::uint32_t rate) noexcept {
            Four(bytes, 0, "RIFF");
            Integer(bytes, 4, static_cast<std::uint32_t>(shape.encodedBytes - 8));
            Four(bytes, 8, "WAVE");
            Four(bytes, 12, "fmt ");
            Integer(bytes, 16, 40);
            Integer(bytes, 20, 0xfffe, 2);
            Integer(bytes, 22, shape.count, 2);
            Integer(bytes, 24, rate);
            Integer(bytes, 28, rate * shape.count * 4);
            Integer(bytes, 32, shape.count * 4, 2);
            Integer(bytes, 34, 32, 2);
            Integer(bytes, 36, 22, 2);
            Integer(bytes, 38, 32, 2);
            Integer(bytes, 40, shape.mask);
            constexpr std::array<std::uint8_t, 16> FloatGuid{3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
            for (std::size_t index = 0; index < FloatGuid.size(); ++index)
                bytes[44 + index] = static_cast<std::byte>(FloatGuid[index]);
            Four(bytes, 60, "fact");
            Integer(bytes, 64, 4);
            Integer(bytes, 68, static_cast<std::uint32_t>(shape.frames));
            Four(bytes, 72, "data");
            Integer(bytes, 76, static_cast<std::uint32_t>(shape.encodedBytes - WaveHeaderBytes));
        }

        /** @brief Encode complete already-rendered PCM with bounded cancellation intervals and no file side effects. */
        Result<std::vector<std::byte>> Encode(const OfflineAudioWaveExportRequest &request, const WaveShape &shape,
                                              const Audio::Backend::OfflineAudioRenderOutput &pcm) {
            using Encoded = Result<std::vector<std::byte>>;
            if (request.render.cancellation.stop_requested())
                return Encoded::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            std::vector<std::byte> bytes(static_cast<std::size_t>(shape.encodedBytes));
            Header(bytes, shape, pcm.format.sampleRate);
            for (std::uint64_t frame = 0; frame < shape.frames; ++frame) {
                if (frame % 4096 == 0 && request.render.cancellation.stop_requested())
                    return Encoded::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
                for (std::size_t channel = 0; channel < shape.count; ++channel) {
                    const auto input = static_cast<std::size_t>(frame) * shape.count + shape.channels[channel].second;
                    const auto output = WaveHeaderBytes + (static_cast<std::size_t>(frame) * shape.count + channel) * 4;
                    Integer(bytes, static_cast<std::size_t>(output), std::bit_cast<std::uint32_t>(pcm.interleaved[input]));
                }
            }
            return Encoded::Success(std::move(bytes));
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
    }  // namespace

    /** @copydoc ExportOfflineAudioWave */
    Result<OfflineAudioWaveExportSummary> ExportOfflineAudioWave(const OfflineAudioWaveExportRequest &request,
                                                                 const Audio::Backend::RenderPort render, DurableFileSystem &files,
                                                                 OfflineAudioWaveExportReceipt &receipt) {
        if (receipt.admitted || receipt.preparedCreated || receipt.preparedRemoved || receipt.publication.WasCommitted())
            return Outcome::Failure(MakeError(FileStorageErrors::Conflict));
        std::optional<ExclusiveFileLock> writer;  // Retained through catch-side cleanup, not unwound before it.
        try {
            if (request.render.cancellation.stop_requested())
                return Outcome::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            const auto shape = Shape(request);
            if (!shape)
                return Outcome::Failure(MakeError(Audio::AudioErrors::FormatCombinationUnsupported));
            if (const auto paths = AdmitPaths(request, files); paths.HasError())
                return Outcome::Failure(paths.ErrorValue());
            const auto lockPath = request.preparedFile.parent_path() / OfflineAudioWaveWriterLock;
            auto acquired = files.TryAcquireExclusive(lockPath, "horo.audio.export");
            if (acquired.HasError())
                return Outcome::Failure(acquired.ErrorValue());
            if (!acquired.Value().ProtectsPath(lockPath))
                return Outcome::Failure(MakeError(FileStorageErrors::UnsafeObject));
            writer.emplace(std::move(acquired).Value());
            receipt.admitted = true;
            auto pcm = Audio::Backend::RenderOfflineAudio(request.render, render);
            if (pcm.HasError())
                return Outcome::Failure(pcm.ErrorValue());
            auto bytes = Encode(request, *shape, pcm.Value());
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            if (request.render.cancellation.stop_requested())
                return Outcome::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            const auto available = files.AvailableBytes(request.preparedFile.parent_path());
            if (available.HasError())
                return Outcome::Failure(available.ErrorValue());
            if (available.Value() < shape->encodedBytes)
                return Outcome::Failure(MakeError(FileStorageErrors::Capacity));
            const auto written = files.WritePrivateDurable(request.preparedFile, bytes.Value(), receipt.preparedCreated);
            if (written.HasError())
                return Fail(written.ErrorValue(), request, files, receipt);
            if (!receipt.preparedCreated)
                return Outcome::Failure(MakeError(FileStorageErrors::IoFailed, "Private export creation was not confirmed"));
            if (request.render.cancellation.stop_requested())
                return Fail(MakeError(Audio::AudioErrors::OperationCancelled), request, files, receipt);
            const auto published = files.AtomicReplaceTracked(request.preparedFile, request.destination, receipt.publication);
            if (published.HasError())
                return Fail(published.ErrorValue(), request, files, receipt);
            if (!receipt.publication.WasCommitted())
                return Fail(MakeError(FileStorageErrors::IoFailed, "Atomic export commit was not confirmed"), request, files, receipt);
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
        return ExportOfflineAudioWave(request, prepared.Value().Port(), files, receipt);
    }
}  // namespace Horo::Application::Internal
