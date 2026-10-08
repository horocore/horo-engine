#include "Horo/Audio/AudioStreamingService.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Audio {
    constexpr std::uint64_t EndMarker = 1ULL << 63U;
    constexpr std::uint64_t CursorMask = EndMarker - 1;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<AudioStreamDecoder *>::is_always_lock_free);

    /** @brief Control-owned cursor for coalesced underrun reporting, never accessed by the callback. */
    struct UnderrunReporting final {
        bool reported{};
        std::uint64_t frames{};
        std::uint64_t callbacks{};
        std::uint64_t sampleFrame{};
    };

    struct AudioStreamState final {
        AudioStreamRequest request;
        std::vector<AudioSample> ring;
        std::vector<AudioSample> decodeBuffer;
        std::vector<std::byte> scratch;
        std::unique_ptr<AudioStreamDecoder> decoder;
        std::atomic<AudioStreamDecoder *> publishedDecoder{nullptr};
        CancellationSource cancellation;
        std::atomic<std::uint64_t> produced{};
        std::atomic<std::uint64_t> consumed{};
        std::atomic<std::uint64_t> underrunFrames{};
        std::atomic<std::uint64_t> underrunCallbacks{};
        // The high bit of produced publishes EOF with its exact final cursor in one release.
        std::atomic<bool> stopped{false};
        bool failed{};
        bool cancelled{};
        // Written only by the fill worker; control reads it after proved job completion.
        bool sourceOpenReturned{};
        std::optional<Error> failure;
        bool portIssued{};
        bool retainedPort{};  // Only control issues/releases the pin; callback never consults it.
        UnderrunReporting reporting;
        std::size_t chargedBytes{};

        explicit AudioStreamState(AudioStreamRequest value, const std::size_t bytes)
            : request(std::move(value)),
              ring(static_cast<std::size_t>(request.ringFrames) * request.decoder.outputFormat.layout.orderedChannels.size()),
              decodeBuffer(static_cast<std::size_t>(request.decoder.maximumFramesPerDecode) *
                           request.decoder.outputFormat.layout.orderedChannels.size()),
              scratch(request.decoder.requiredWorkingBytes), chargedBytes(bytes) {}
    };

    struct AudioStreamingService::Slot final {
        std::unique_ptr<AudioStreamState> state;
        std::optional<JobHandle> fill;
        std::uint64_t generation{1};
        /** @brief Reaps a proved terminal job; returns whether its storage is still in use. */
        bool Reap();
        /** @brief Tests whether this idle stream needs a prioritized lookahead fill. */
        bool NeedsFill() const;
        /** @brief Selects the highest-priority eligible slot, retaining stable ties. */
        static Slot *Select(std::span<Slot> slots);
    };

    namespace {
        [[nodiscard]] bool Terminal(const JobState state) noexcept {
            using enum JobState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        /** @brief Checks decoder bounds before ring sizing arithmetic. */
        [[nodiscard]] bool ValidDecoderSpec(const AudioStreamDecoderSpec &spec) noexcept {
            return spec.codec.IsValid() && ValidateAudioProcessingFormat(spec.outputFormat) && spec.maximumFramesPerDecode != 0 &&
                   spec.maximumFramesPerDecode <= MaximumAudioCallbackFrames && spec.requiredWorkingBytes <= (8U << 20U) &&
                   spec.frameCount <= (1ULL << 40U) && !spec.outputFormat.layout.orderedChannels.empty() &&
                   spec.outputFormat.layout.orderedChannels.size() <= MaximumAudioChannels;
        }

        /** @brief Checks buffering and package ceilings independently of decoder validity. */
        [[nodiscard]] bool ValidBufferPolicy(const AudioStreamRequest &request) noexcept {
            return request.ringFrames >= request.decoder.maximumFramesPerDecode * 2ULL &&
                   request.ringFrames <= MaximumAudioCallbackFrames * 64ULL && request.lookaheadFrames != 0 &&
                   request.lookaheadFrames <= request.ringFrames && request.maximumPackageBytes != 0 &&
                   request.maximumPackageBytes <= (128U << 20U) &&
                   (request.underrunPolicy == AudioStreamUnderrunPolicy::ContinueWithSilence ||
                    request.underrunPolicy == AudioStreamUnderrunPolicy::StopWithSilence);
        }

        /** @brief Validates admission and computes its bounded storage charge. */
        [[nodiscard]] bool ValidRequest(const AudioStreamRequest &request, const AudioStreamingLimits &limits,
                                        std::size_t &chargedBytes) noexcept {
            const auto &spec = request.decoder;
            const std::size_t channels = spec.outputFormat.layout.orderedChannels.size();
            if (!request.asset.IsValid() || !ValidDecoderSpec(spec) || !ValidBufferPolicy(request))
                return false;
            const std::uint64_t samples = (static_cast<std::uint64_t>(request.ringFrames) + spec.maximumFramesPerDecode) * channels;
            const std::uint64_t bytes = samples * sizeof(AudioSample) + spec.requiredWorkingBytes + request.maximumPackageBytes;
            if (bytes > limits.maximumBytes)
                return false;
            chargedBytes = static_cast<std::size_t>(bytes);
            return true;
        }

        /** @brief Requires the opened decoder to honor every admitted field. */
        bool MatchesSpec(const AudioStreamDecoderSpec &actual, const AudioStreamDecoderSpec &expected) {
            return actual.codec == expected.codec && actual.outputFormat == expected.outputFormat &&
                   actual.frameCount == expected.frameCount && actual.maximumFramesPerDecode == expected.maximumFramesPerDecode &&
                   actual.requiredWorkingBytes == expected.requiredWorkingBytes && actual.seekable == expected.seekable;
        }

        /** @brief Opens once and publishes cancellation access without changing decoder ownership. */
        Result<void> EnsureDecoder(AudioStreamState &state, const AudioStreamPackageSource &source, const CancellationToken &cancellation) {
            if (!state.decoder) {
                // Foundation's job boundary contains provider exceptions, including non-standard ones.
                auto opened = source.open(source.context, state.request.asset, state.request.decoder, state.request.maximumPackageBytes,
                                          cancellation);
                state.sourceOpenReturned = true;
                if (cancellation.IsCancellationRequested())
                    return JobCancelled();
                if (opened.HasError())
                    return Result<void>::Failure(std::move(opened).ErrorValue());
                auto candidate = std::make_unique<AudioStreamDecoder>(std::move(opened).Value());
                if (!MatchesSpec(candidate->Spec(), state.request.decoder))
                    return Result<void>::Failure(MakeError(AudioErrors::StreamReadFailed));
                state.decoder = std::move(candidate);
                state.publishedDecoder.store(state.decoder.get(), std::memory_order_seq_cst);
                // Together with Stop's SC pointer load and Foundation's SC cancellation operations,
                // both misses would require publish < check < request < load < publish in SC order.
                // Thus Stop cancels this decoder, or this check does so before provider Decode.
                if (cancellation.IsCancellationRequested()) {
                    state.decoder->Cancel();
                    return JobCancelled();
                }
            }
            return Result<void>::Success();
        }

        /** @brief Rejects invalid decoded blocks and canonicalizes silence before publication. */
        Result<void> NormalizeSamples(const std::span<AudioSample> samples) {
            for (auto &sample : samples) {
                if (!std::isfinite(sample))
                    return Result<void>::Failure(MakeError(AudioErrors::StreamReadFailed));
                if (sample == 0.0F || std::fpclassify(sample) == FP_SUBNORMAL)
                    sample = 0.0F;
            }
            return Result<void>::Success();
        }

        /** @brief Copies one validated block and publishes data with its exact terminal cursor. */
        void PublishBlock(AudioStreamState &state, const std::uint64_t written, const AudioStreamDecodedBlock progress) {
            const std::size_t channels = state.request.decoder.outputFormat.layout.orderedChannels.size();
            for (std::uint32_t frame = 0; frame < progress.frames; ++frame) {
                const std::size_t destination = static_cast<std::size_t>((written + frame) % state.request.ringFrames) * channels;
                std::copy_n(state.decodeBuffer.data() + static_cast<std::size_t>(frame) * channels, channels,
                            state.ring.data() + destination);
            }
            state.produced.store((written + progress.frames) | (progress.endOfStream ? EndMarker : 0), std::memory_order_seq_cst);
        }

        /** @brief Performs one cancellable bounded worker fill. */
        [[nodiscard]] Result<void> Fill(AudioStreamState &state, const AudioStreamPackageSource &source,
                                        const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested() || state.stopped.load(std::memory_order_seq_cst))
                return JobCancelled();
            if (auto ready = EnsureDecoder(state, source, cancellation); ready.HasError())
                return ready;

            const std::uint64_t read = state.consumed.load(std::memory_order_seq_cst);
            const std::uint64_t written = state.produced.load(std::memory_order_seq_cst) & CursorMask;
            const auto buffered = written >= read ? std::min<std::uint64_t>(written - read, state.request.ringFrames) : 0;
            const auto freeFrames = static_cast<std::uint32_t>(state.request.ringFrames - buffered);
            const auto frames = std::min(freeFrames, state.request.decoder.maximumFramesPerDecode);
            if (frames == 0)
                return Result<void>::Success();
            auto decoded = state.decoder->Decode(state.decodeBuffer, state.scratch, frames);
            if (cancellation.IsCancellationRequested() || state.stopped.load(std::memory_order_seq_cst))
                return JobCancelled();
            if (decoded.HasError())
                return Result<void>::Failure(std::move(decoded).ErrorValue());

            const auto progress = decoded.Value();
            const std::size_t channels = state.request.decoder.outputFormat.layout.orderedChannels.size();
            if (auto normalized =
                    NormalizeSamples(std::span(state.decodeBuffer).first(static_cast<std::size_t>(progress.frames) * channels));
                normalized.HasError())
                return normalized;
            PublishBlock(state, written, progress);
            return Result<void>::Success();
        }

        /** @brief Checks callback arguments without touching worker-owned data. */
        bool ValidOutput(const AudioStreamState *state, const std::span<AudioSample *const> planes, const std::uint32_t frames) noexcept {
            return state != nullptr && frames != 0 && frames <= MaximumAudioCallbackFrames &&
                   planes.size() == state->request.decoder.outputFormat.layout.orderedChannels.size() &&
                   std::ranges::all_of(planes, [](const auto *plane) {
                return plane != nullptr;
            });
        }

        /** @brief Copies acquired ring samples and pads the remainder with positive zero. */
        void CopyOutput(const AudioStreamState &state, const std::span<AudioSample *const> planes, const std::uint64_t read,
                        const std::uint32_t available, const std::uint32_t frames) noexcept {
            for (std::uint32_t frame = 0; frame < available; ++frame) {
                const auto offset = static_cast<std::size_t>((read + frame) % state.request.ringFrames) * planes.size();
                for (std::size_t channel = 0; channel < planes.size(); ++channel)
                    planes[channel][frame] = state.ring[offset + channel];
            }
            for (auto *plane : planes)
                std::fill_n(plane + available, frames - available, 0.0F);
        }

        /** @brief Records saturating counters on the single callback consumer and applies its underrun policy. */
        void RecordUnderrun(AudioStreamState &state, const std::uint32_t missing) noexcept {
            const auto oldFrames = state.underrunFrames.load(std::memory_order_seq_cst);
            state.underrunFrames.store(oldFrames > std::numeric_limits<std::uint64_t>::max() - missing
                                           ? std::numeric_limits<std::uint64_t>::max()
                                           : oldFrames + missing,
                                       std::memory_order_seq_cst);
            const auto oldCallbacks = state.underrunCallbacks.load(std::memory_order_seq_cst);
            state.underrunCallbacks.store(oldCallbacks == std::numeric_limits<std::uint64_t>::max() ? oldCallbacks : oldCallbacks + 1,
                                          std::memory_order_seq_cst);
            if (state.request.underrunPolicy == AudioStreamUnderrunPolicy::StopWithSilence)
                state.stopped.store(true, std::memory_order_seq_cst);
        }

        /** @brief Checks service storage and join bounds before allocation. */
        bool ValidLimits(const AudioStreamingLimits &limits) noexcept {
            return limits.maximumStreams != 0 && limits.maximumStreams <= 65'536 && limits.maximumConcurrentFills != 0 &&
                   limits.maximumConcurrentFills <= limits.maximumStreams && limits.maximumBytes != 0 &&
                   limits.maximumBytes <= (256U << 20U) && limits.joinTimeoutMilliseconds != 0;
        }
    }  // namespace

    /** @copydoc AudioStreamRenderPort::~AudioStreamRenderPort */
    AudioStreamRenderPort::~AudioStreamRenderPort() {
        if (retained_ && state_ != nullptr) {
            state_->retainedPort = false;
            state_->portIssued = false;
        }
    }

    /** @copydoc AudioStreamRenderPort::Render */
    AudioStreamRenderResult AudioStreamRenderPort::Render(const std::span<AudioSample *const> planes,
                                                          const std::uint32_t frames) const noexcept {
        AudioStreamRenderResult result;
        if (!ValidOutput(state_, planes, frames))
            return result;
        const bool stopped = state_->stopped.load(std::memory_order_seq_cst);
        const std::uint64_t read = state_->consumed.load(std::memory_order_seq_cst);
        const std::uint64_t publication = state_->produced.load(std::memory_order_seq_cst);
        const std::uint64_t written = publication & CursorMask;
        const auto available = stopped || written < read ? 0U : static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, written - read));
        CopyOutput(*state_, planes, read, available, frames);
        state_->consumed.store(read + available, std::memory_order_seq_cst);

        const bool ended = (publication & EndMarker) != 0 && read + available == written;
        const std::uint32_t missing = frames - available;
        if (missing != 0 && !stopped && !ended)
            RecordUnderrun(*state_, missing);
        result.availableFrames = available;
        result.silentFrames = missing;
        result.ended = ended;
        result.stopped = state_->stopped.load(std::memory_order_seq_cst);
        return result;
    }

    /** @copydoc AudioStreamingService::AudioStreamingService */
    AudioStreamingService::AudioStreamingService(ConstructionKey, JobSystem &jobs, AudioStreamPackageSource source,
                                                 const AudioStreamingLimits limits)
        : jobs_(jobs), source_(std::move(source)), limits_(limits), slots_(limits.maximumStreams) {}

    /** @copydoc AudioStreamingService::Create */
    Result<std::unique_ptr<AudioStreamingService>> AudioStreamingService::Create(JobSystem &jobs, AudioStreamPackageSource source,
                                                                                 const AudioStreamingLimits limits) {
        if (!source.context.IsValid() || source.open == nullptr || !ValidLimits(limits))
            return Result<std::unique_ptr<AudioStreamingService>>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        try {
            return Result<std::unique_ptr<AudioStreamingService>>::Success(
                std::make_unique<AudioStreamingService>(ConstructionKey{}, jobs, std::move(source), limits));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<AudioStreamingService>>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        }
    }

    /** @copydoc AudioStreamingService::~AudioStreamingService */
    AudioStreamingService::~AudioStreamingService() {
        if (Shutdown().HasError())
            std::terminate();  // A live worker may still retain callback-visible storage.
    }

    /** @copydoc AudioStreamingService::Admit */
    Result<AudioStreamHandle> AudioStreamingService::Admit(AudioStreamRequest request) {
        std::size_t bytes = 0;
        if (closed_ || !ValidRequest(request, limits_, bytes))
            return Result<AudioStreamHandle>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        if (bytes > limits_.maximumBytes - reservedBytes_)
            return Result<AudioStreamHandle>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        for (std::uint32_t index = 0; index < limits_.maximumStreams; ++index) {
            auto &slot = slots_[index];
            if (slot.state != nullptr || slot.generation == 0)
                continue;
            try {
                slot.state = std::make_unique<AudioStreamState>(std::move(request), bytes);
            } catch (const std::bad_alloc &) {
                return Result<AudioStreamHandle>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
            }
            reservedBytes_ += bytes;
            return Result<AudioStreamHandle>::Success({index + 1, slot.generation});
        }
        return Result<AudioStreamHandle>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
    }

    AudioStreamState *AudioStreamingService::Find(const AudioStreamHandle handle) noexcept {
        if (!handle.IsValid() || handle.slot > limits_.maximumStreams)
            return nullptr;
        const auto &slot = slots_[handle.slot - 1];
        return slot.generation == handle.generation ? slot.state.get() : nullptr;
    }

    const AudioStreamState *AudioStreamingService::Find(const AudioStreamHandle handle) const noexcept {
        if (!handle.IsValid() || handle.slot > limits_.maximumStreams)
            return nullptr;
        const auto &slot = slots_[handle.slot - 1];
        return slot.generation == handle.generation ? slot.state.get() : nullptr;
    }

    /** @copydoc AudioStreamingService::RenderPort */
    Result<AudioStreamRenderPort> AudioStreamingService::RenderPort(const AudioStreamHandle handle) {
        return IssueRenderPort(handle, false);
    }

    /** @copydoc AudioStreamingService::RetainedRenderPort */
    Result<AudioStreamRenderPort> AudioStreamingService::RetainedRenderPort(const AudioStreamHandle handle) {
        return IssueRenderPort(handle, true);
    }

    /** @copydoc AudioStreamingService::IssueRenderPort */
    Result<AudioStreamRenderPort> AudioStreamingService::IssueRenderPort(const AudioStreamHandle handle, const bool retained) {
        auto *state = Find(handle);
        if (state == nullptr)
            return Result<AudioStreamRenderPort>::Failure(MakeError(AudioErrors::HandleStale));
        if (state->portIssued)
            return Result<AudioStreamRenderPort>::Failure(MakeError(AudioErrors::VoiceAdmissionClosed));
        state->portIssued = true;
        state->retainedPort = retained;
        return Result<AudioStreamRenderPort>::Success(AudioStreamRenderPort(state, retained));
    }

    /** @copydoc AudioStreamingService::DecoderSpec */
    Result<AudioStreamDecoderSpec> AudioStreamingService::DecoderSpec(const AudioStreamHandle handle) const {
        const auto *state = Find(handle);
        if (state == nullptr)
            return Result<AudioStreamDecoderSpec>::Failure(MakeError(AudioErrors::HandleStale));
        return Result<AudioStreamDecoderSpec>::Success(state->request.decoder);
    }

    /** @copydoc AudioStreamingService::Slot::Reap */
    bool AudioStreamingService::Slot::Reap() {
        if (!fill)
            return false;
        const auto job = fill->Snapshot();
        if (!job) {
            state->failed = true;
            return true;  // Retain the handle and storage when completion cannot be proved.
        }
        if (!Terminal(job->state))
            return true;
        if (job->terminalResult && job->terminalResult->error) {
            if (job->state == JobState::Cancelled)
                state->cancelled = true;
            else {
                state->failed = true;
                const auto &failure = *job->terminalResult->error;
                state->failure = state->sourceOpenReturned ? failure : WrapError(AudioErrors::StreamReadFailed, failure);
            }
        }
        fill.reset();
        return false;
    }

    /** @copydoc AudioStreamingService::Slot::NeedsFill */
    bool AudioStreamingService::Slot::NeedsFill() const {
        if (!state || fill || state->failed || state->cancelled || state->stopped.load(std::memory_order_seq_cst) ||
            state->cancellation.Token().IsCancellationRequested())
            return false;
        if (const auto publication = state->produced.load(std::memory_order_seq_cst); (publication & EndMarker) != 0)
            return false;
        const auto read = state->consumed.load(std::memory_order_seq_cst);
        const auto written = state->produced.load(std::memory_order_seq_cst) & CursorMask;
        const auto buffered = written >= read ? written - read : 0;
        return buffered < state->request.lookaheadFrames && buffered < state->request.ringFrames;
    }

    /** @copydoc AudioStreamingService::Slot::Select */
    AudioStreamingService::Slot *AudioStreamingService::Slot::Select(const std::span<Slot> slots) {
        Slot *selected = nullptr;
        for (auto &slot : slots) {
            if (!slot.NeedsFill())
                continue;
            if (selected == nullptr || slot.state->request.priority > selected->state->request.priority)
                selected = &slot;
        }
        return selected;
    }

    /** @copydoc AudioStreamingService::Pump */
    void AudioStreamingService::Pump() {
        if (closed_)
            return;
        std::uint32_t active = 0;
        for (std::uint32_t index = 0; index < limits_.maximumStreams; ++index) {
            auto &slot = slots_[index];
            if (slot.state && slot.state->stopped.load(std::memory_order_seq_cst) &&
                !slot.state->cancellation.Token().IsCancellationRequested())
                (void)Stop({index + 1, slot.generation});
            if (slot.Reap())
                ++active;
        }
        while (active < limits_.maximumConcurrentFills) {
            Slot *selected = Slot::Select(slots_);
            if (selected == nullptr)
                return;  // No eligible work remains in this pump.
            auto *state = selected->state.get();
            JobDescriptor descriptor;
            descriptor.parentCancellation = state->cancellation.Token();
            auto submitted = jobs_.SubmitResult(std::move(descriptor), [state, source = source_](const CancellationToken &token) {
                return Fill(*state, source, token);
            });
            if (submitted.HasError())
                break;  // Scheduler overload is retried by the next control pump.
            selected->fill.emplace(std::move(submitted).Value());
            ++active;
        }
    }

    /** @copydoc AudioStreamingService::Stop */
    Result<void> AudioStreamingService::Stop(const AudioStreamHandle handle) {
        auto *state = Find(handle);
        if (state == nullptr)
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        state->stopped.store(true, std::memory_order_seq_cst);
        state->cancellation.RequestCancellation();
        if (auto *decoder = state->publishedDecoder.load(std::memory_order_seq_cst); decoder != nullptr)
            decoder->Cancel();
        if (auto &fill = slots_[handle.slot - 1].fill; fill)
            (void)fill->RequestCancel();
        return Result<void>::Success();
    }

    /** @copydoc AudioStreamingService::Retire */
    Result<void> AudioStreamingService::Retire(const AudioStreamHandle handle) {
        const auto *state = Find(handle);
        if (state == nullptr)
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        if (state->retainedPort)
            return Result<void>::Failure(MakeError(AudioErrors::VoiceAdmissionClosed));
        (void)Stop(handle);
        auto &slot = slots_[handle.slot - 1];
        if (slot.fill) {
            const auto joined =
                slot.fill->Wait({WaitPolicy::OwnerThreadBlockAllowed, Duration::FromMilliseconds(limits_.joinTimeoutMilliseconds)});
            if (const auto snapshot = slot.fill->Snapshot(); !snapshot || !Terminal(snapshot->state))
                return joined.HasError() ? joined : Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
            slot.fill.reset();
        }
        reservedBytes_ -= state->chargedBytes;
        slot.state.reset();
        if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            slot.generation = 0;
        else
            ++slot.generation;
        return Result<void>::Success();
    }

    /** @copydoc AudioStreamingService::Snapshot */
    Result<AudioStreamSnapshot> AudioStreamingService::Snapshot(const AudioStreamHandle handle) const {
        const auto *state = Find(handle);
        if (state == nullptr)
            return Result<AudioStreamSnapshot>::Failure(MakeError(AudioErrors::HandleStale));
        const auto read = state->consumed.load(std::memory_order_seq_cst);
        const auto publication = state->produced.load(std::memory_order_seq_cst);
        const auto written = publication & CursorMask;
        const auto buffered = written >= read ? std::min<std::uint64_t>(written - read, state->request.ringFrames) : 0;
        return Result<AudioStreamSnapshot>::Success(
            {static_cast<std::uint32_t>(buffered), state->underrunFrames.load(std::memory_order_seq_cst),
             state->underrunCallbacks.load(std::memory_order_seq_cst), (publication & EndMarker) != 0, state->failed, state->cancelled,
             state->stopped.load(std::memory_order_seq_cst), state->failure});
    }

    /** @copydoc AudioStreamingService::TakeUnderrunReport */
    Result<std::optional<AudioStreamUnderrunReport>> AudioStreamingService::TakeUnderrunReport(const AudioStreamHandle handle,
                                                                                               const std::uint64_t sampleFrame,
                                                                                               const std::uint64_t minimumIntervalFrames) {
        auto *state = Find(handle);
        if (state == nullptr)
            return Result<std::optional<AudioStreamUnderrunReport>>::Failure(MakeError(AudioErrors::HandleStale));
        if (minimumIntervalFrames == 0)
            return Result<std::optional<AudioStreamUnderrunReport>>::Failure(MakeError(AudioErrors::StreamCapacityExceeded));
        const auto frames = state->underrunFrames.load(std::memory_order_seq_cst);
        auto &reporting = state->reporting;
        if (frames == reporting.frames ||
            (reporting.reported && sampleFrame >= reporting.sampleFrame && sampleFrame - reporting.sampleFrame < minimumIntervalFrames))
            return Result<std::optional<AudioStreamUnderrunReport>>::Success(std::nullopt);
        const auto callbacks = state->underrunCallbacks.load(std::memory_order_seq_cst);
        AudioStreamUnderrunReport report{frames - reporting.frames, callbacks - reporting.callbacks, sampleFrame};
        reporting.frames = frames;
        reporting.callbacks = callbacks;
        reporting.sampleFrame = sampleFrame;
        reporting.reported = true;
        return Result<std::optional<AudioStreamUnderrunReport>>::Success(report);
    }

    /** @copydoc AudioStreamingService::Shutdown */
    Result<void> AudioStreamingService::Shutdown() {
        if (shutdownComplete_)
            return Result<void>::Success();
        closed_ = true;
        for (std::uint32_t index = 0; index < limits_.maximumStreams; ++index) {
            if (slots_[index].state)
                (void)Stop({index + 1, slots_[index].generation});
        }
        for (std::uint32_t index = 0; index < limits_.maximumStreams; ++index) {
            if (slots_[index].state) {
                const auto retired = Retire({index + 1, slots_[index].generation});
                if (retired.HasError())
                    return retired;
            }
        }
        shutdownComplete_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Audio
