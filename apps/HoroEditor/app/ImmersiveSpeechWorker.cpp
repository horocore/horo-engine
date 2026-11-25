#include "ImmersiveSpeechWorker.h"

#include "Horo/Security/SecureMemory.h"

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

namespace Horo::Editor {
    namespace {
        using enum SpeechTranscriptionStatus;
        namespace Capture = Audio::Backend;
        constexpr auto IdlePollInterval = std::chrono::milliseconds(10);
        constexpr unsigned MaximumBlocksPerPoll = 4;

        /** @brief Relays Audio revocation to the independently owned worker stop token. */
        struct RevokeWorker final {
            std::stop_source target;

            void operator()() noexcept {
                target.request_stop();
            }
        };

        /** @brief Owns one worker scratch buffer and scrubs samples on consumption and unwind. */
        struct PcmScratch final {
            std::vector<Audio::AudioSample> values;

            ~PcmScratch() {
                Clear();
            }

            void Clear() noexcept {
                Security::SecureZero(std::as_writable_bytes(std::span{values}));
            }
        };

        /** @brief Confirms that the detached tail belongs to the same source and format as the live queue. */
        bool SameSource(const Capture::AudioCaptureBufferDescriptor &expected, const Capture::AudioCaptureBufferDescriptor &actual) {
            return expected.device == actual.device && expected.generation == actual.generation &&
                   expected.clockDomain == actual.clockDomain && expected.format == actual.format &&
                   expected.maximumFrames == actual.maximumFrames;
        }
    }  // namespace

    /** @brief Shared storage, with one writer and one reader of PCM.
     * @details Control owns the live writer and initializes the tail before publishing sealed. The worker
     * reads the tail only after acquiring sealed and draining the live queue. A separate mutex covers only
     * the single latest partial. Control polls that mutex with try_lock and never waits on the worker.
     */
    struct ImmersiveSpeechWorker::State final {
        Capture::AudioCaptureBuffer live;
        std::stop_source cancel;
        std::stop_callback<RevokeWorker> revoke;
        std::optional<Capture::AudioCaptureBuffer> tail;
        std::atomic<bool> sealed{};
        std::uint64_t liveSequence{};
        std::mutex partialMutex;
        std::optional<SpeechTranscriptionUpdate> partial;

        State(Capture::AudioCaptureBuffer queue, const std::stop_token captureRevoked)
            : live(std::move(queue)), revoke(captureRevoked, RevokeWorker{cancel}) {}

        /** @brief Scrubs the block after each synchronous provider operation. */
        static SpeechTranscriptionStatus Drain(Capture::AudioCaptureBuffer &source, ImmersiveSpeechTranscription &stream,
                                               PcmScratch &scratch, const std::stop_token stop) {
            for (unsigned index = 0; index < MaximumBlocksPerPoll && !stop.stop_requested(); ++index) {
                Capture::AudioCapturedBlock block{};
                const auto read = source.TryRead(block, scratch.values);
                if (read == Capture::AudioCaptureRead::Empty)
                    return Pending;
                if (read != Capture::AudioCaptureRead::Copied)
                    return CaptureInterrupted;
                const auto &descriptor = *source.Descriptor();
                const auto samples = std::span{scratch.values}.first(block.frames * descriptor.format.layout.orderedChannels.size());
                const auto result = stream.Append(descriptor, block, samples);
                scratch.Clear();
                if (result != Accepted)
                    return result;
            }
            return stop.stop_requested() ? Cancelled : Accepted;
        }

        /** @brief Retains the latest draft or its withdrawal. Terminals use the separate one-shot future. */
        void Publish(SpeechTranscriptionUpdate update) {
            const std::lock_guard lock{partialMutex};
            partial = std::move(update);
        }

        /** @brief Retires captured samples after the worker has exited and before completion is delivered. */
        void RetirePcm() noexcept {
            static_cast<void>(Capture::AudioCaptureBuffer{std::move(live)});
            tail.reset();
            partial.reset();
        }

        /** @brief Rechecks the lease before handing a retired worker's result to the caller. */
        SpeechTranscriptionUpdate Complete(std::optional<SpeechTranscriptionUpdate> reply) {
            RetirePcm();
            if (cancel.stop_requested())
                return {Cancelled, {}};
            if (!reply)
                return {ProviderUnavailable, {}};
            if (reply->evidence && !reply->evidence->IsCurrent())
                return {StaleContext, {}};
            return std::move(*reply);
        }

        /** @brief Drains live PCM first, then the detached tail and exactly one input seal. */
        SpeechTranscriptionStatus Feed(ImmersiveSpeechTranscription &stream, PcmScratch &scratch, const std::stop_token stop,
                                       bool &inputFinished) {
            if (inputFinished)
                return Pending;
            const bool detached = sealed.load();
            const auto liveStatus = Drain(live, stream, scratch, stop);
            if (!detached || liveStatus != Pending)
                return liveStatus;
            const auto tailStatus = Drain(*tail, stream, scratch, stop);
            if (tailStatus != Pending)
                return tailStatus;
            const auto finished = stream.FinishInput();
            inputFinished = finished == Accepted;
            return finished;
        }

        /** @brief Drives one provider to completion, cancellation, or its bounded deadline. */
        SpeechTranscriptionUpdate Run(ImmersiveSpeechTranscription &stream, ISpeechProvider &provider, const std::stop_token workerStop) {
            const std::stop_callback relay{workerStop, RevokeWorker{cancel}};
            if (const auto started = stream.Start(provider); started != Accepted)
                return {started, {}};
            const auto &descriptor = *live.Descriptor();
            PcmScratch scratch{std::vector<Audio::AudioSample>(descriptor.maximumFrames * descriptor.format.layout.orderedChannels.size())};
            bool inputFinished{};
            const auto stop = cancel.get_token();
            while (!stop.stop_requested()) {
                const auto fed = Feed(stream, scratch, stop, inputFinished);
                if (fed != Pending && fed != Accepted)
                    return {fed, {}};
                auto update = stream.Poll();
                if (!stream.IsActive())
                    return update;
                if (update.evidence || update.status == DraftWithdrawn)
                    Publish(std::move(update));
                if (fed == Pending)
                    std::this_thread::sleep_for(IdlePollInterval);
            }
            return {Cancelled, {}};
        }
    };

    ImmersiveSpeechWorker::~ImmersiveSpeechWorker() {
        Cancel();
    }

    /** @copydoc ImmersiveSpeechWorker::Start */
    SpeechTranscriptionStatus ImmersiveSpeechWorker::Start(const ImmersiveAuthoringContext &owner, const ImmersivePlanningContext &context,
                                                           const Capture::AudioCaptureBufferDescriptor &source,
                                                           const std::stop_token captureRevoked, std::shared_ptr<ISpeechProvider> provider,
                                                           const std::string_view locale, const float minimumConfidence) {
        if (Busy())
            return Closed;
        if (!provider || !captureRevoked.stop_possible())
            return InvalidRequest;
        Cancel();
        try {
            auto queue = Capture::AudioCaptureBuffer::Create(source);
            if (queue.HasError())
                return InvalidRequest;
            auto shared = std::make_shared<State>(std::move(queue).Value(), captureRevoked);
            auto prepared =
                ImmersiveSpeechTranscription::Prepare(owner, context, source, locale, minimumConfidence, shared->cancel.get_token());
            if (!prepared.stream)
                return prepared.status;
            if (!worker_.Start(Agent::AgentBackgroundTurn<SpeechTranscriptionUpdate>::Operation{
                    [shared, provider = std::move(provider), stream = std::move(prepared.stream)](std::stop_token stop) {
                return shared->Run(*stream, *provider, stop);
            }}))
                return ProviderUnavailable;
            state_ = std::move(shared);
            return Accepted;
        } catch (const std::bad_alloc &) {
            return ResourceUnavailable;
        }
    }

    /** @copydoc ImmersiveSpeechWorker::Submit */
    SpeechTranscriptionStatus ImmersiveSpeechWorker::Submit(const Capture::AudioCapturedBlock &block,
                                                            const std::span<const Audio::AudioSample> pcm) noexcept {
        if (!state_ || !Busy() || state_->sealed.load())
            return Closed;
        if (state_->cancel.stop_requested())
            return Cancelled;
        const bool continuous =
            block.sequence == state_->liveSequence + 1 && block.droppedFramesBefore == 0 && block.missingSourceFramesBefore == 0;
        if (!continuous ||
            state_->live.TryWrite(block.start, block.firstFrame, block.frames, pcm) != Capture::AudioCaptureWrite::Accepted) {
            Cancel();
            return CaptureInterrupted;
        }
        state_->liveSequence = block.sequence;
        return Accepted;
    }

    /** @copydoc ImmersiveSpeechWorker::Seal */
    SpeechTranscriptionStatus ImmersiveSpeechWorker::Seal(Capture::AudioCaptureBuffer tail) noexcept {
        if (!state_ || !Busy() || state_->sealed.load())
            return Closed;
        if (state_->cancel.stop_requested())
            return Cancelled;
        const auto *source = tail.Descriptor();
        if (!source || !SameSource(*state_->live.Descriptor(), *source) || tail.DroppedFrames() != 0) {
            Cancel();
            return CaptureInterrupted;
        }
        state_->tail.emplace(std::move(tail));
        state_->sealed.store(true);
        return Accepted;
    }

    /** @copydoc ImmersiveSpeechWorker::Poll */
    SpeechTranscriptionUpdate ImmersiveSpeechWorker::Poll() {
        if (auto completion = worker_.Poll())
            return state_->Complete(std::move(completion->reply));
        if (!state_ || !Busy() || state_->cancel.stop_requested())
            return {Pending, {}};
        const std::unique_lock lock{state_->partialMutex, std::try_to_lock};
        if (!lock || !state_->partial)
            return {Pending, {}};
        auto update = std::exchange(state_->partial, std::nullopt);
        if (update->evidence && !update->evidence->IsCurrent())
            return {StaleContext, {}};
        return std::move(*update);
    }

    /** @copydoc ImmersiveSpeechWorker::OwnsEvidence */
    bool ImmersiveSpeechWorker::OwnsEvidence(const ImmersiveSpeechEvidence &evidence) const noexcept {
        return state_ && evidence.stop_ == state_->cancel.get_token() && evidence.IsCurrent();
    }

    /** @copydoc ImmersiveSpeechWorker::Cancel */
    void ImmersiveSpeechWorker::Cancel() noexcept {
        if (state_)
            state_->cancel.request_stop();
        worker_.Cancel();
    }

    /** @copydoc ImmersiveSpeechWorker::Busy */
    bool ImmersiveSpeechWorker::Busy() const noexcept {
        return worker_.Busy();
    }
}  // namespace Horo::Editor
