#include "EditorMicrophoneRecording.h"

#include "Horo/Audio/AudioErrors.h"
#include "Horo/Security/SecureMemory.h"

#include <algorithm>
#include <chrono>
#include <new>
#include <system_error>

namespace Horo::Editor {
    namespace {
        using namespace Application::Internal;
        using RecordResult = Result<Assets::AssetRecord>;
        using SaveResult = Result<std::string>;

        /** @brief Limits a take to 20 seconds or 8 MiB of PCM, whichever is smaller. */
        std::uint64_t RecordingFrames(const Audio::AudioProcessingFormat &format) noexcept {
            constexpr std::uint64_t MaximumPcmBytes = 8U << 20U;
            const auto bytesPerFrame = format.layout.orderedChannels.size() * sizeof(Audio::AudioSample);
            return bytesPerFrame == 0 ? 0 : std::min<std::uint64_t>(format.sampleRate * 20ULL, MaximumPcmBytes / bytesPerFrame);
        }

        /** @brief Reads the exact recorded identity from a private registry built and persisted by the normal publisher. */
        RecordResult PublishTake(EditorRecordingTake take, Audio::Backend::AudioCaptureBuffer tail, ProjectMutationCoordinator &mutations,
                                 DurableFileSystem &files, const CancellationToken &stop) {
            Assets::AssetRegistry workerRegistry;
            Application::ProjectAssetImportCommitter publisher{mutations, files, &workerRegistry};
            if (const auto result = take.recording.Publish(std::move(tail), take.destination, publisher, stop); result.HasError())
                return RecordResult::Failure(result.ErrorValue());
            const auto snapshot = workerRegistry.Snapshot();
            const auto *asset = snapshot.Find(take.destination.id);
            return asset ? RecordResult::Success(*asset) : RecordResult::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        }

        /** @brief Adds a durable new record to the live snapshot without reverting unrelated registry changes. */
        SaveResult AdoptRecording(Assets::AssetRegistry &registry, Assets::AssetRecord asset) {
            const auto path = asset.sourcePath.String();
            const auto snapshot = registry.Snapshot();
            if (const auto *existing = snapshot.Find(asset.id))
                return existing->sourcePath.String() == path ? SaveResult::Success(path)
                                                             : SaveResult::Failure(MakeError(Audio::AudioErrors::IdentityInvalid));
            auto records = std::vector(snapshot.Records().begin(), snapshot.Records().end());
            records.push_back(std::move(asset));
            if (registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Failed)
                return SaveResult::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
            return SaveResult::Success(path);
        }
    }  // namespace

    EditorRecordingTake::EditorRecordingTake(Audio::Backend::AudioCaptureBufferDescriptor descriptor, AudioRecordingDestination target,
                                             AudioRecording accumulation)
        : capture(std::move(descriptor)), destination(std::move(target)), recording(std::move(accumulation)),
          scratch(capture.maximumFrames * capture.format.layout.orderedChannels.size()) {}

    EditorRecordingTake::~EditorRecordingTake() {
        Security::SecureZero(std::as_writable_bytes(std::span{scratch}));
    }

    /** @copydoc EditorRecordingTake::Prepare */
    Result<EditorRecordingTake> EditorRecordingTake::Prepare(const Audio::Backend::AudioCaptureBufferDescriptor &capture,
                                                             AudioRecordingDestination destination) {
        using Outcome = Result<EditorRecordingTake>;
        auto recording = AudioRecording::Prepare(capture, RecordingFrames(capture.format));
        if (recording.HasError())
            return Outcome::Failure(recording.ErrorValue());
        try {
            return Outcome::Success(EditorRecordingTake{capture, std::move(destination), std::move(recording).Value()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc EditorRecordingTake::Append */
    Result<void> EditorRecordingTake::Append(const Audio::Backend::AudioCapturedBlock &block) {
        if (block.frames > capture.maximumFrames)
            return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        const auto count = block.frames * capture.format.layout.orderedChannels.size();
        const auto samples = std::span{scratch}.first(count);
        auto result = recording.Append(capture, block, samples);
        Security::SecureZero(std::as_writable_bytes(samples));
        return result;
    }

    EditorRecordingPublication::EditorRecordingPublication(ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                           Assets::AssetRegistry &registry)
        : mutations_(mutations), files_(files), registry_(registry) {}

    EditorRecordingPublication::~EditorRecordingPublication() {
        Cancel();
    }

    /** @copydoc EditorRecordingPublication::Start */
    bool EditorRecordingPublication::Start(EditorRecordingTake take, Audio::Backend::AudioCaptureBuffer tail) {
        if (Busy())
            return false;
        try {
            CancellationSource cancellation;
            const auto token = cancellation.Token();
            std::packaged_task<RecordResult()> task{
                [take = std::move(take), tail = std::move(tail), &mutations = mutations_, &files = files_, token]() mutable {
                return PublishTake(std::move(take), std::move(tail), mutations, files, token);
            }};
            auto job = task.get_future();
            worker_ = std::jthread([task = std::move(task)]() mutable {
                task.make_ready_at_thread_exit();
            });
            cancellation_.emplace(std::move(cancellation));
            job_ = std::move(job);
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        } catch (const std::system_error &) {
            return false;
        }
    }

    /** @copydoc EditorRecordingPublication::Cancel */
    void EditorRecordingPublication::Cancel() noexcept {
        if (cancellation_)
            cancellation_->RequestCancellation();
    }

    /** @copydoc EditorRecordingPublication::Busy */
    bool EditorRecordingPublication::Busy() const noexcept {
        return job_.valid();
    }

    /** @copydoc EditorRecordingPublication::Poll */
    std::optional<SaveResult> EditorRecordingPublication::Poll() {
        if (!Busy() || job_.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
            return std::nullopt;
        const auto stop = cancellation_->Token();
        cancellation_.reset();
        try {
            auto saved = job_.get();
            if (stop.IsCancellationRequested())
                return SaveResult::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
            if (saved.HasError())
                return SaveResult::Failure(saved.ErrorValue());
            return AdoptRecording(registry_, std::move(saved).Value());
        } catch (const std::bad_alloc &) {
            return SaveResult::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        } catch (const std::future_error &) {
            return SaveResult::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        } catch (const std::system_error &) {
            return SaveResult::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        }
    }
}  // namespace Horo::Editor
