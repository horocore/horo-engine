#pragma once

/** @file ImmersiveSpeechTranscription.h @brief Worker-side speech evidence for one immersive listening window. */

#include "Horo/Audio/Internal/AudioCaptureBuffer.h"
#include "ImmersiveAuthoringContext.h"

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <variant>

namespace Horo::Editor {
    enum class SpeechHypothesisKind : std::uint8_t {
        Partial,
        Final
    };

    /** @brief Borrowed provider output. The provider retains the text until its next operation. */
    struct SpeechHypothesisView final {
        SpeechHypothesisKind kind{};
        Audio::AudioMonotonicTimestamp start;
        Audio::AudioMonotonicTimestamp end;
        float confidence{};
        std::string_view text;
    };

    struct SpeechProviderUnavailable final {};

    struct SpeechProviderInvalidTranscript final {};

    using SpeechProviderPoll =
        std::variant<std::monostate, SpeechHypothesisView, SpeechProviderUnavailable, SpeechProviderInvalidTranscript>;

    /** @brief One host-admitted provider stream. All calls are serialized on a worker outside Audio and XR callbacks.
     * @details Implementations bound their own work, honor cancellation, and never retain caller spans. Transport,
     * credentials, and permission to send audio off-device belong to the host-selected adapter. Adapter
     * failures use the typed outcomes. Destruction releases the adapter after its final result or cancellation.
     */
    class ISpeechProviderStream {
    public:
        virtual ~ISpeechProviderStream() = default;
        /** @brief Appends one admitted PCM block. False indicates stream failure; the caller never retries it. */
        [[nodiscard]] virtual bool Append(const Audio::Backend::AudioCapturedBlock &block, std::span<const Audio::AudioSample> pcm,
                                          std::stop_token stop) noexcept = 0;
        /** @brief Signals an explicit end of speech input. False indicates stream failure. */
        [[nodiscard]] virtual bool FinishInput(std::stop_token stop) noexcept = 0;
        /** @brief Reads at most one update. Empty indicates pending work. */
        [[nodiscard]] virtual SpeechProviderPoll Poll(std::stop_token stop) noexcept = 0;
        /** @brief Cancels pending work and closes all audio admission for this provider stream. */
        virtual void Cancel() noexcept = 0;
    };

    /** @brief Host-selected factory for independent speech provider streams. */
    class ISpeechProvider {
    public:
        virtual ~ISpeechProvider() = default;
        /** @brief Starts the selected locale and source format. Empty indicates unavailable or unsupported service.
         * @details The format and locale are borrowed only for this call. The adapter owns its stream and limits.
         */
        [[nodiscard]] virtual std::unique_ptr<ISpeechProviderStream> Open(std::string_view locale,
                                                                          const Audio::AudioProcessingFormat &format,
                                                                          std::stop_token stop) noexcept = 0;
    };

    enum class SpeechTranscriptionStatus : std::uint8_t {
        Accepted,
        Pending,
        Finished,
        Closed,
        StaleContext,
        Cancelled,
        CaptureInterrupted,
        InvalidRequest,
        CaptureLimitExceeded,
        ProviderTimedOut,
        HypothesisLimitExceeded,
        ResourceUnavailable,
        ProviderUnavailable,
        InvalidTranscript,
        LowConfidence,
        DraftWithdrawn /**< A newer partial fell below threshold. Recognition can continue. */
    };

    class ImmersiveSpeechTranscription;

    /** @brief Owned, bounded text evidence. Consumers check IsCurrent again before adopting it on the editor lane. */
    class ImmersiveSpeechEvidence final {
    public:
        static constexpr std::size_t MaximumTextBytes = 4096;
        ImmersiveSpeechEvidence() = default;
        ImmersiveSpeechEvidence(const ImmersiveSpeechEvidence &) = default;
        ImmersiveSpeechEvidence &operator=(const ImmersiveSpeechEvidence &) = default;
        ImmersiveSpeechEvidence(ImmersiveSpeechEvidence &&) noexcept = default;
        ImmersiveSpeechEvidence &operator=(ImmersiveSpeechEvidence &&) noexcept = default;
        /** @brief Clears the owned transcript bytes before releasing the evidence. */
        ~ImmersiveSpeechEvidence() noexcept;

        /** @brief Checks the context generation and both cancellation authorities before adoption. */
        [[nodiscard]] bool IsCurrent() const noexcept;

        [[nodiscard]] const ImmersivePlanningContext &Context() const & noexcept {
            return context_;
        }

        [[nodiscard]] const ImmersivePlanningContext &Context() const && = delete;

        [[nodiscard]] std::string_view Locale() const & noexcept {
            return {locale_.data(), localeSize_};
        }

        [[nodiscard]] std::string_view Locale() const && = delete;
        /** @brief Borrows the complete hypothesis only while its evidence is current. */
        [[nodiscard]] SpeechHypothesisView Hypothesis() const & noexcept;
        [[nodiscard]] SpeechHypothesisView Hypothesis() const && = delete;
        /** @brief Borrows local display text only while the evidence is live and of the requested kind. */
        [[nodiscard]] std::string_view PreviewText(SpeechHypothesisKind kind) const & noexcept;
        [[nodiscard]] std::string_view PreviewText(SpeechHypothesisKind kind) const && = delete;

        /** @brief Returns the duration of PCM admitted to recognition, while this evidence remains current. */
        [[nodiscard]] std::optional<std::chrono::nanoseconds> AdmittedAudioDuration() const noexcept;

    private:
        friend class ImmersiveSpeechTranscription;
        friend class ImmersiveSpeechWorker;
        static constexpr std::size_t MaximumLocaleBytes = 35;
        ImmersivePlanningContext context_;
        std::stop_token release_;
        std::stop_token stop_;
        std::array<char, MaximumTextBytes> text_{};
        std::array<char, MaximumLocaleBytes> locale_{};
        std::size_t textSize_{};
        std::size_t localeSize_{};
        SpeechHypothesisKind kind_{};
        Audio::AudioMonotonicTimestamp start_;
        Audio::AudioMonotonicTimestamp end_;
        std::chrono::nanoseconds admittedAudioDuration_{};
        float confidence_{};
    };

    /** @brief The result of polling one provider update. */
    struct SpeechTranscriptionUpdate final {
        SpeechTranscriptionStatus status{};
        std::optional<ImmersiveSpeechEvidence> evidence; /**< Present only for an admitted partial or final. */
    };

    /** @brief Owner-lane preparation result. A successful stream can start on the worker. */
    struct SpeechTranscriptionStart final {
        SpeechTranscriptionStatus status{};
        std::unique_ptr<ImmersiveSpeechTranscription> stream;
    };

    /** @brief Serializes one provider session over a single Audio-owned capture generation.
     * @details Prepare runs on the Editor owner lane for context admission. Provider operations are serialized on a worker.
     * The host checks consent before copying from Audio and signals its stop token on mute, revocation or device loss.
     * Audio bytes are borrowed for Append only. There is no replay, history, automatic command dispatch or approval.
     */
    class ImmersiveSpeechTranscription final {
        /** @brief Allows allocation only after the preparation factory admits the context and source. */
        class ConstructionKey final {
            friend class ImmersiveSpeechTranscription;
            ConstructionKey() = default;

        public:
            ConstructionKey(const ConstructionKey &) = default;
        };

    public:
        /** @brief Maximum duration of live microphone input, independent of provider finalization time. */
        static constexpr std::chrono::seconds MaximumInputDuration{15};
        static constexpr std::chrono::milliseconds MaximumProviderDuration{20'000};

        /** @brief Factory-only construction for an admitted context and checked source. */
        ImmersiveSpeechTranscription(ConstructionKey, const ImmersivePlanningContext &context,
                                     const Audio::Backend::AudioCaptureBufferDescriptor &source, std::string_view locale,
                                     float minimumConfidence, std::stop_token stop, std::chrono::milliseconds providerBudget);
        /** @brief Binds the admitted context, exact source and selected provider to a bounded listening window.
         * @param owner Current owner of the planning snapshot. @param context Exact snapshot to admit.
         * @param source Audio-owned source generation and format. @param locale Recognizer language tag.
         * @param minimumConfidence Inclusive threshold in [0, 1].
         * @param stop Host cancellation that also revokes previously issued evidence.
         * @param providerBudget Positive elapsed-time limit within MaximumProviderDuration.
         * @return Stream or typed admission failure.
         */
        [[nodiscard]] static SpeechTranscriptionStart Prepare(const ImmersiveAuthoringContext &owner,
                                                              const ImmersivePlanningContext &context,
                                                              const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                              std::string_view locale, float minimumConfidence, std::stop_token stop = {},
                                                              std::chrono::milliseconds providerBudget = MaximumProviderDuration);
        ~ImmersiveSpeechTranscription();
        ImmersiveSpeechTranscription(const ImmersiveSpeechTranscription &) = delete;
        ImmersiveSpeechTranscription &operator=(const ImmersiveSpeechTranscription &) = delete;

        /** @brief Starts one host-selected adapter on the worker without retrying a failed attempt. */
        [[nodiscard]] SpeechTranscriptionStatus Start(ISpeechProvider &provider);
        /** @brief Submits one ordered block and cancels on discontinuity, a mismatched source, or budget exhaustion.
         * @param source Exact Audio capture descriptor for the supplied block.
         * @param block Audio timing and gap metadata.
         * @param pcm Interleaved samples borrowed for this call only.
         * @return Accepted when the provider consumes the block, or the typed interruption or failure.
         */
        [[nodiscard]] SpeechTranscriptionStatus Append(const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                       const Audio::Backend::AudioCapturedBlock &block,
                                                       std::span<const Audio::AudioSample> pcm);
        /** @brief Seals the current admitted utterance before any final hypothesis can be issued. */
        [[nodiscard]] SpeechTranscriptionStatus FinishInput();
        /** @brief Takes one provider update and rejects malformed, stale, or low-confidence evidence explicitly. */
        [[nodiscard]] SpeechTranscriptionUpdate Poll();

        /** @brief Reports whether polling may still produce another update on this worker lane. */
        [[nodiscard]] bool IsActive() const noexcept {
            return status_ == SpeechTranscriptionStatus::Accepted;
        }

        /** @brief Stops the stream and revokes evidence already issued by it. Safe to repeat on the worker lane. */
        void Cancel() noexcept;

    private:
        friend class ImmersiveSpeechWorker;
        struct ProviderWindow;
        /** @brief Counts PCM accepted by the provider independently of the current hypothesis. Worker lane only. */
        [[nodiscard]] std::chrono::nanoseconds AudioDuration() const noexcept;
        [[nodiscard]] SpeechTranscriptionStatus CheckLive() noexcept;
        [[nodiscard]] SpeechTranscriptionStatus CheckProvider(bool accepted) noexcept;
        SpeechTranscriptionStatus CloseWith(SpeechTranscriptionStatus status) noexcept;
        [[nodiscard]] SpeechTranscriptionUpdate Admit(const SpeechHypothesisView &hypothesis);
        [[nodiscard]] bool MatchesSource(const Audio::Backend::AudioCaptureBufferDescriptor &source) const noexcept;
        [[nodiscard]] bool ContinuesCapture(const Audio::Backend::AudioCapturedBlock &block) const noexcept;
        /** @brief Requires the exact next sample position, allowing either integral nanosecond rounding of that boundary. */
        [[nodiscard]] bool MatchesCaptureClock(const Audio::Backend::AudioCapturedBlock &block) const noexcept;
        [[nodiscard]] bool FitsWindow(const Audio::Backend::AudioCapturedBlock &block, std::uint64_t end) const noexcept;
        [[nodiscard]] bool ValidTiming(const SpeechHypothesisView &hypothesis) const noexcept;
        [[nodiscard]] bool ValidHypothesis(const SpeechHypothesisView &hypothesis) const noexcept;

        ImmersiveSpeechEvidence evidence_;
        Audio::Backend::AudioCaptureBufferDescriptor source_;
        std::stop_source release_{std::nostopstate}; /**< Revocation source for the last published hypothesis. */
        std::unique_ptr<ISpeechProviderStream> stream_;
        std::unique_ptr<ProviderWindow> providerWindow_;
        std::chrono::steady_clock::time_point deadline_{};
        std::chrono::milliseconds providerBudget_;
        std::uint64_t nextFrame_{};
        std::uint64_t sequence_{};
        std::uint64_t firstTime_{};
        std::uint64_t endTime_{};
        std::uint32_t hypotheses_{};
        float minimumConfidence_{};
        SpeechTranscriptionStatus status_{SpeechTranscriptionStatus::Pending};
        bool inputFinished_{};
    };
}  // namespace Horo::Editor
