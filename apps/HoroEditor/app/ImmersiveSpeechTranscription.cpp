#include "ImmersiveSpeechTranscription.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Security/SecureMemory.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace Horo::Editor {
    namespace {
        using enum SpeechTranscriptionStatus;
        using Clock = std::chrono::steady_clock;
        constexpr std::uint64_t NanosecondsPerSecond = 1'000'000'000;
        constexpr auto MaximumInputSeconds = static_cast<std::uint64_t>(ImmersiveSpeechTranscription::MaximumInputDuration.count());
        constexpr std::uint32_t MaximumHypotheses = 64;

        /** @brief Accepts a bounded ASCII language tag with nonempty hyphen-separated subtags. */
        bool ValidLocale(const std::string_view locale) noexcept {
            if (locale.empty() || locale.front() == '-' || locale.back() == '-' || locale.find("--") != std::string_view::npos)
                return false;
            constexpr std::string_view Allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
            return locale.find_first_not_of(Allowed) == std::string_view::npos;
        }

        /** @brief Bounds each block and verifies its exact interleaved finite PCM shape. */
        bool ValidPcm(const Audio::Backend::AudioCaptureBufferDescriptor &source, const std::uint32_t frames,
                      const std::span<const Audio::AudioSample> pcm) noexcept {
            return frames != 0 && frames <= source.maximumFrames && pcm.size() == frames * source.format.layout.orderedChannels.size() &&
                   std::ranges::all_of(pcm, [](const auto sample) {
                return std::isfinite(sample);
            });
        }

        /** @brief Checks the exact Audio capture generation and processing bounds once before dispatch. */
        bool ValidSource(const Audio::Backend::AudioCaptureBufferDescriptor &source) noexcept {
            return source.device.IsValid() && source.generation != 0 && source.clockDomain != 0 && source.maximumFrames != 0 &&
                   source.maximumFrames <= Audio::MaximumAudioCallbackFrames && Audio::ValidateAudioProcessingFormat(source.format);
        }

        /** @brief Computes the exclusive time end with no wrap, including fractional-frame rounding. */
        std::optional<std::uint64_t> BlockEnd(const Audio::Backend::AudioCapturedBlock &block, const std::uint32_t rate) noexcept {
            const auto duration = (block.frames * NanosecondsPerSecond + rate - 1) / rate;
            if (block.start.nanoseconds > std::numeric_limits<std::uint64_t>::max() - duration)
                return std::nullopt;
            return block.start.nanoseconds + duration;
        }

        /** @brief Accepts confidence only within the provider-neutral normalized range. */
        bool ValidConfidence(const float confidence) noexcept {
            return std::isfinite(confidence) && confidence >= 0.0F && confidence <= 1.0F;
        }

        /** @brief Validates text without copying any provider-owned bytes. */
        bool ValidText(const std::string_view text) noexcept {
            return !text.empty() && text.size() <= ImmersiveSpeechEvidence::MaximumTextBytes && text.find('\0') == std::string_view::npos &&
                   IsValidUtf8ScalarSequence(text);
        }
    }  // namespace

    /** @brief Signals elapsed-time or host cancellation during a synchronous provider call. */
    struct ImmersiveSpeechTranscription::ProviderWindow final {
        struct RelayStop final {
            std::stop_source target;

            void operator()() noexcept {
                target.request_stop();
            }
        };

        std::stop_source providerStop;
        std::mutex mutex;
        std::condition_variable_any wake;
        std::stop_callback<RelayStop> relay;
        std::jthread timer;

        ProviderWindow(const Clock::time_point deadline, const std::stop_token parent)
            : relay(parent, RelayStop{providerStop}), timer([this, deadline](const std::stop_token done) {
                  std::unique_lock lock{mutex};
                  wake.wait_until(lock, done, deadline, [] {
                      return false;
                  });
                  lock.unlock();
                  if (!done.stop_requested())
                      providerStop.request_stop();
              }) {}
    };

    /** @copydoc ImmersiveSpeechEvidence::~ImmersiveSpeechEvidence */
    ImmersiveSpeechEvidence::~ImmersiveSpeechEvidence() noexcept {
        Security::SecureZero(std::as_writable_bytes(std::span{text_}));
    }

    /** @copydoc ImmersiveSpeechEvidence::IsCurrent */
    bool ImmersiveSpeechEvidence::IsCurrent() const noexcept {
        return context_.IsCurrent() && !release_.stop_requested() && !stop_.stop_requested();
    }

    SpeechHypothesisView ImmersiveSpeechEvidence::Hypothesis() const & noexcept {
        return {kind_, start_, end_, confidence_, {text_.data(), textSize_}};
    }

    /** @copydoc ImmersiveSpeechEvidence::PreviewText */
    std::string_view ImmersiveSpeechEvidence::PreviewText(const SpeechHypothesisKind kind) const & noexcept {
        return kind_ == kind && IsCurrent() ? std::string_view{text_.data(), textSize_} : std::string_view{};
    }

    ImmersiveSpeechTranscription::ImmersiveSpeechTranscription(ConstructionKey, const ImmersivePlanningContext &context,
                                                               const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                               const std::string_view locale, const float minimumConfidence,
                                                               const std::stop_token stop, const std::chrono::milliseconds providerBudget)
        : source_(source), providerBudget_(providerBudget), minimumConfidence_(minimumConfidence) {
        evidence_.context_ = context;
        evidence_.release_ = release_.get_token();
        evidence_.stop_ = stop;
        evidence_.localeSize_ = locale.size();
        std::ranges::copy(locale, evidence_.locale_.begin());
    }

    ImmersiveSpeechTranscription::~ImmersiveSpeechTranscription() {
        if (stream_)
            Cancel();
    }

    /** @copydoc ImmersiveSpeechTranscription::Prepare */
    SpeechTranscriptionStart ImmersiveSpeechTranscription::Prepare(const ImmersiveAuthoringContext &owner,
                                                                   const ImmersivePlanningContext &context,
                                                                   const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                                   const std::string_view locale, const float minimumConfidence,
                                                                   const std::stop_token stop,
                                                                   const std::chrono::milliseconds providerBudget) {
        if (const auto admission = owner.Admit(context, ImmersiveRequestKind::SpeechCapture); admission != ImmersiveAdmission::Admitted)
            return {admission == ImmersiveAdmission::StaleContext ? StaleContext : InvalidRequest, {}};
        if (stop.stop_requested())
            return {Cancelled, {}};
        if (!ValidSource(source) || !ValidConfidence(minimumConfidence) || locale.size() > ImmersiveSpeechEvidence::MaximumLocaleBytes ||
            !ValidLocale(locale) || providerBudget <= std::chrono::milliseconds::zero() || providerBudget > MaximumProviderDuration)
            return {InvalidRequest, {}};
        return {Accepted, std::make_unique<ImmersiveSpeechTranscription>(ConstructionKey{}, context, source, locale, minimumConfidence,
                                                                         stop, providerBudget)};
    }

    /** @copydoc ImmersiveSpeechTranscription::Start */
    SpeechTranscriptionStatus ImmersiveSpeechTranscription::Start(ISpeechProvider &provider) {
        if (const auto status = CheckLive(); status != Pending)
            return status == Accepted ? InvalidRequest : status;
        deadline_ = Clock::now() + providerBudget_;
        providerWindow_ = std::make_unique<ProviderWindow>(deadline_, evidence_.stop_);
        status_ = Accepted;
        stream_ = provider.Open(evidence_.Locale(), source_.format, providerWindow_->providerStop.get_token());
        return CheckProvider(static_cast<bool>(stream_));
    }

    /** @copydoc ImmersiveSpeechTranscription::Append */
    SpeechTranscriptionStatus ImmersiveSpeechTranscription::Append(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                                   const Audio::Backend::AudioCapturedBlock &block,
                                                                   const std::span<const Audio::AudioSample> pcm) {
        if (const auto status = CheckLive(); status != Accepted)
            return status;
        if (inputFinished_)
            return Closed;
        if (!MatchesSource(source) || !ContinuesCapture(block))
            return CloseWith(CaptureInterrupted);
        if (!ValidPcm(source_, block.frames, pcm))
            return CloseWith(InvalidRequest);
        const auto end = BlockEnd(block, source_.format.sampleRate);
        if (!end || !FitsWindow(block, *end))
            return CloseWith(LimitExceeded);
        const auto accepted = stream_->Append(block, pcm, providerWindow_->providerStop.get_token());
        if (const auto status = CheckProvider(accepted); status != Accepted)
            return status;
        if (sequence_ == 0)
            firstTime_ = block.start.nanoseconds;
        lastStart_ = block.start.nanoseconds;
        endTime_ = *end;
        nextFrame_ += block.frames;
        sequence_ = block.sequence;
        return Accepted;
    }

    /** @copydoc ImmersiveSpeechTranscription::FinishInput */
    SpeechTranscriptionStatus ImmersiveSpeechTranscription::FinishInput() {
        if (const auto status = CheckLive(); status != Accepted)
            return status;
        if (inputFinished_)
            return Accepted;
        if (sequence_ == 0)
            return CloseWith(CaptureInterrupted);
        const auto accepted = stream_->FinishInput(providerWindow_->providerStop.get_token());
        if (const auto status = CheckProvider(accepted); status != Accepted)
            return status;
        inputFinished_ = true;
        return Accepted;
    }

    /** @copydoc ImmersiveSpeechTranscription::Poll */
    SpeechTranscriptionUpdate ImmersiveSpeechTranscription::Poll() {
        if (const auto status = CheckLive(); status != Accepted)
            return {status, {}};
        const auto update = stream_->Poll(providerWindow_->providerStop.get_token());
        if (const auto status = CheckLive(); status != Accepted)
            return {status, {}};
        if (const auto *hypothesis = std::get_if<SpeechHypothesisView>(&update))
            return Admit(*hypothesis);
        if (std::holds_alternative<SpeechProviderUnavailable>(update))
            return {CloseWith(ProviderUnavailable), {}};
        if (std::holds_alternative<SpeechProviderInvalidTranscript>(update))
            return {CloseWith(InvalidTranscript), {}};
        return {Pending, {}};
    }

    /** @copydoc ImmersiveSpeechTranscription::Cancel */
    void ImmersiveSpeechTranscription::Cancel() noexcept {
        CloseWith(Cancelled);
    }

    SpeechTranscriptionStatus ImmersiveSpeechTranscription::CheckLive() noexcept {
        if (status_ != Accepted && status_ != Pending)
            return status_;
        if (evidence_.stop_.stop_requested())
            return CloseWith(Cancelled);
        if (!evidence_.context_.IsCurrent())
            return CloseWith(StaleContext);
        if (status_ == Accepted && Clock::now() >= deadline_)
            return CloseWith(LimitExceeded);
        return status_;
    }

    SpeechTranscriptionStatus ImmersiveSpeechTranscription::CheckProvider(const bool accepted) noexcept {
        if (const auto status = CheckLive(); status != Accepted)
            return status;
        return accepted ? Accepted : CloseWith(ProviderUnavailable);
    }

    SpeechTranscriptionStatus ImmersiveSpeechTranscription::CloseWith(const SpeechTranscriptionStatus status) noexcept {
        release_.request_stop();
        if (stream_) {
            stream_->Cancel();
            stream_.reset();
        }
        providerWindow_.reset();
        status_ = status;
        return status_;
    }

    SpeechTranscriptionUpdate ImmersiveSpeechTranscription::Admit(const SpeechHypothesisView &hypothesis) {
        using enum SpeechHypothesisKind;
        const bool isFinal = hypothesis.kind == Final;
        if (++hypotheses_ > MaximumHypotheses)
            return {CloseWith(LimitExceeded), {}};
        if (!ValidHypothesis(hypothesis))
            return {CloseWith(InvalidTranscript), {}};
        if (hypothesis.confidence < minimumConfidence_)
            return {isFinal ? CloseWith(LowConfidence) : DraftWithdrawn, {}};

        auto owned = evidence_;
        owned.kind_ = hypothesis.kind;
        owned.start_ = hypothesis.start;
        owned.end_ = hypothesis.end;
        owned.confidence_ = hypothesis.confidence;
        owned.textSize_ = hypothesis.text.size();
        std::ranges::copy(hypothesis.text, owned.text_.begin());
        if (isFinal) {
            providerWindow_.reset();
            stream_.reset();
            status_ = Finished;
        }
        return {status_, std::move(owned)};
    }

    bool ImmersiveSpeechTranscription::MatchesSource(const Audio::Backend::AudioCaptureBufferDescriptor &source) const noexcept {
        return source.device == source_.device && source.generation == source_.generation && source.clockDomain == source_.clockDomain &&
               source.format == source_.format && source.maximumFrames == source_.maximumFrames;
    }

    bool ImmersiveSpeechTranscription::ContinuesCapture(const Audio::Backend::AudioCapturedBlock &block) const noexcept {
        return block.start.clockDomain == source_.clockDomain && block.sequence == sequence_ + 1 && block.firstFrame == nextFrame_ &&
               (sequence_ == 0 || block.start.nanoseconds > lastStart_) && block.missingSourceFramesBefore == 0 &&
               block.droppedFramesBefore == 0;
    }

    bool ImmersiveSpeechTranscription::FitsWindow(const Audio::Backend::AudioCapturedBlock &block, const std::uint64_t end) const noexcept {
        const auto first = sequence_ == 0 ? block.start.nanoseconds : firstTime_;
        return end >= endTime_ && end - first <= MaximumInputSeconds * NanosecondsPerSecond &&
               nextFrame_ + block.frames <= MaximumInputSeconds * source_.format.sampleRate;
    }

    bool ImmersiveSpeechTranscription::ValidTiming(const SpeechHypothesisView &hypothesis) const noexcept {
        return sequence_ != 0 && hypothesis.start.clockDomain == source_.clockDomain && hypothesis.end.clockDomain == source_.clockDomain &&
               firstTime_ <= hypothesis.start.nanoseconds && hypothesis.start.nanoseconds < hypothesis.end.nanoseconds &&
               hypothesis.end.nanoseconds <= endTime_;
    }

    bool ImmersiveSpeechTranscription::ValidHypothesis(const SpeechHypothesisView &hypothesis) const noexcept {
        using enum SpeechHypothesisKind;
        const bool allowedKind = hypothesis.kind == Partial || (hypothesis.kind == Final && inputFinished_);
        return allowedKind && ValidText(hypothesis.text) && ValidTiming(hypothesis) && ValidConfidence(hypothesis.confidence);
    }
}  // namespace Horo::Editor
