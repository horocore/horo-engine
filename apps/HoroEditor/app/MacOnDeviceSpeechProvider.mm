#include "MacOnDeviceSpeechProvider.h"

#include "Horo/Security/SecureMemory.h"

#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace Horo::Editor {
    namespace {
        // The authorization callback owns no Editor state. It only clears this process-wide prompt flag.
        std::atomic<bool> permissionPending{};
        constexpr double NanosecondsPerSecond = 1'000'000'000.0;
        constexpr double MaximumTimestampSeconds = ImmersiveSpeechTranscription::MaximumInputDuration.count();
        constexpr NSUInteger MaximumSegments = 256;

        /** @brief Avoids touching Speech on executables that cannot display the OS consent explanation. */
        bool HasSpeechUsageDescription() noexcept {
            id entry = [NSBundle.mainBundle objectForInfoDictionaryKey:@"NSSpeechRecognitionUsageDescription"];
            return [entry isKindOfClass:NSString.class] && [(NSString *)entry length] != 0;
        }

        /** @brief Binds one framework callback to the latest result, with no Editor or worker references.
         * @details The mutex protects the single strong result and terminal flags between the framework callback
         * and the worker. Both release the lock before native task operations. Audio/XR callbacks never enter here.
         */
        class RecognitionMailbox final {
        public:
            /** @brief Replaces a partial or records the first terminal result. Late callbacks are ignored. */
            void Publish(SFSpeechRecognitionResult *result, NSError *error) noexcept {
                const std::lock_guard lock{mutex_};
                if (closed_ || terminal_)
                    return;
                latest_ = result;
                failed_ = error != nil && !result.isFinal;
                terminal_ = failed_ || result.isFinal;
            }

            /** @brief Takes one result. Holds an early final until the worker explicitly seals input. */
            SFSpeechRecognitionResult *Read(const bool sealed, bool &failed) noexcept {
                const std::lock_guard lock{mutex_};
                failed = failed_;
                if (!sealed && latest_.isFinal)
                    return nil;
                SFSpeechRecognitionResult *result = latest_;
                latest_ = nil;
                return result;
            }

            /** @brief Rejects later callbacks and releases queued recognition data before native cancellation. */
            void Close() noexcept {
                const std::lock_guard lock{mutex_};
                closed_ = true;
                latest_ = nil;
            }

        private:
            std::mutex mutex_;
            __strong SFSpeechRecognitionResult *latest_{nil};
            bool closed_{};
            bool terminal_{};
            bool failed_{};
        };

        /** @brief Maps Speech's audio-relative offset into the exact admitted Audio clock without overflow. */
        std::optional<Audio::AudioMonotonicTimestamp> MapTimestamp(const Audio::AudioMonotonicTimestamp origin,
                                                                   const double offset) noexcept {
            if (!std::isfinite(offset) || offset < 0.0 || offset > MaximumTimestampSeconds)
                return std::nullopt;
            const auto delta = static_cast<std::uint64_t>(std::llround(offset * NanosecondsPerSecond));
            if (delta > std::numeric_limits<std::uint64_t>::max() - origin.nanoseconds)
                return std::nullopt;
            return Audio::AudioMonotonicTimestamp{origin.clockDomain, origin.nanoseconds + delta};
        }

        /** @brief Copies no more than the worker's transcript budget in canonical UTF-8. */
        std::size_t CopyTranscript(NSString *text, const std::span<char> output) noexcept {
            const auto size = [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            if (size == 0 || size > output.size())
                return 0;
            NSData *encoded = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
            if (encoded.length != size)
                return 0;
            const std::span source{static_cast<const char *>(encoded.bytes), static_cast<std::size_t>(size)};
            std::ranges::copy(source, output.begin());
            return source.size();
        }

        /** @brief Uses the least confident segment, without inventing confidence for partial hypotheses. */
        float TranscriptConfidence(NSArray<SFTranscriptionSegment *> *segments) noexcept {
            float confidence = 1.0F;
            for (SFTranscriptionSegment *segment in segments) {
                const auto value = segment.confidence;
                if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
                    return value;
                confidence = std::min(confidence, value);
            }
            return confidence;
        }

        /** @brief Owns one native request. There is no microphone or transport owned by this adapter. */
        class MacSpeechStream final : public ISpeechProviderStream {
        public:
            explicit MacSpeechStream(AVAudioFormat *format) : mailbox_(std::make_shared<RecognitionMailbox>()), format_(format) {}

            ~MacSpeechStream() override {
                Cancel();
            }

            /** @brief Requires on-device support before creating a recognition task. */
            bool Begin(SFSpeechRecognizer *recognizer) noexcept {
                if (!recognizer.isAvailable || !recognizer.supportsOnDeviceRecognition ||
                    ObserveMacSpeechAuthorization() != MacSpeechAuthorization::Granted)
                    return false;
                recognizer_ = recognizer;
                request_ = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
                request_.requiresOnDeviceRecognition = YES;
                request_.shouldReportPartialResults = YES;
                const auto mailbox = mailbox_;
                task_ = [recognizer_ recognitionTaskWithRequest:request_
                                                  resultHandler:^(SFSpeechRecognitionResult *result, NSError *error) {
                                                    mailbox->Publish(result, error);
                                                  }];
                return task_ != nil;
            }

            /** @copydoc ISpeechProviderStream::Append */
            bool Append(const Audio::Backend::AudioCapturedBlock &block, const std::span<const Audio::AudioSample> pcm,
                        const std::stop_token stop) noexcept override {
                @autoreleasepool {
                    if (sealed_ || !Authorized(stop) || !ValidBlock(block, pcm.size()))
                        return false;
                    AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format_ frameCapacity:block.frames];
                    if (!buffer.floatChannelData)
                        return false;
                    buffer.frameLength = block.frames;
                    CopyPcm(buffer, pcm);
                    if (!origin_)
                        origin_ = block.start;
                    [request_ appendAudioPCMBuffer:buffer];
                    framesSubmitted_ += block.frames;
                    return true;
                }
            }

            /** @copydoc ISpeechProviderStream::FinishInput */
            bool FinishInput(const std::stop_token stop) noexcept override {
                @autoreleasepool {
                    if (!Authorized(stop) || !origin_)
                        return false;
                    if (!std::exchange(sealed_, true))
                        [request_ endAudio];
                    return true;
                }
            }

            /** @copydoc ISpeechProviderStream::Poll */
            SpeechProviderPoll Poll(const std::stop_token stop) noexcept override {
                ClearTranscript();
                @autoreleasepool {
                    if (!Authorized(stop))
                        return SpeechProviderUnavailable{};
                    bool failed{};
                    SFSpeechRecognitionResult *result = mailbox_->Read(sealed_, failed);
                    if (failed)
                        return SpeechProviderUnavailable{};
                    if (!result)
                        return {};
                    return View(result);
                }
            }

            /** @copydoc ISpeechProviderStream::Cancel */
            void Cancel() noexcept override {
                ClearTranscript();
                @autoreleasepool {
                    mailbox_->Close();
                    [task_ cancel];
                    task_ = nil;
                    request_ = nil;
                    recognizer_ = nil;
                }
            }

        private:
            /** @brief Clears the adapter-owned text after its borrowed view expires. */
            void ClearTranscript() noexcept {
                Security::SecureZero(std::as_writable_bytes(std::span{text_}));
            }

            /** @brief Checks revocation before every worker handoff. */
            bool Authorized(const std::stop_token stop) const noexcept {
                return request_ != nil && !stop.stop_requested() && recognizer_.supportsOnDeviceRecognition &&
                       ObserveMacSpeechAuthorization() == MacSpeechAuthorization::Granted;
            }

            /** @brief Admits only the negotiated block shape and the same Audio clock. */
            bool ValidBlock(const Audio::Backend::AudioCapturedBlock &block, const std::size_t samples) const noexcept {
                const auto maximumFrames = static_cast<std::uint64_t>(format_.sampleRate) *
                                           static_cast<std::uint64_t>(ImmersiveSpeechTranscription::MaximumInputDuration.count());
                return block.frames != 0 && block.frames <= Audio::MaximumAudioCallbackFrames && block.start.clockDomain != 0 &&
                       (!origin_ || block.start.clockDomain == origin_->clockDomain) &&
                       samples == static_cast<std::size_t>(block.frames) * format_.channelCount &&
                       framesSubmitted_ + block.frames <= maximumFrames;
            }

            /** @brief Copies the interleaved Audio block into a per-call planar native buffer. */
            void CopyPcm(AVAudioPCMBuffer *buffer, const std::span<const Audio::AudioSample> pcm) const noexcept {
                auto *const planes = buffer.floatChannelData;
                const std::size_t channels = format_.channelCount;
                for (std::size_t frame = 0; frame < buffer.frameLength; ++frame)
                    for (std::size_t channel = 0; channel < channels; ++channel)
                        planes[channel][frame] = pcm[frame * channels + channel];
            }

            /** @brief Projects bounded native text, confidence and segment timing into a borrowed hypothesis. */
            SpeechProviderPoll View(SFSpeechRecognitionResult *result) noexcept {
                SFTranscription *transcription = result.bestTranscription;
                NSArray<SFTranscriptionSegment *> *segments = transcription.segments;
                if (!origin_ || segments.count == 0)
                    return MissingTiming(result.isFinal);
                if (segments.count > MaximumSegments)
                    return SpeechProviderInvalidTranscript{};
                const auto start = MapTimestamp(*origin_, segments.firstObject.timestamp);
                SFTranscriptionSegment *last = segments.lastObject;
                const auto end = MapTimestamp(*origin_, last.timestamp + last.duration);
                if (!start || !end)
                    return SpeechProviderInvalidTranscript{};
                if (start->nanoseconds >= end->nanoseconds)
                    return MissingTiming(result.isFinal);
                const auto size = CopyTranscript(transcription.formattedString, text_);
                if (size == 0)
                    return SpeechProviderInvalidTranscript{};
                return SpeechHypothesisView{result.isFinal ? SpeechHypothesisKind::Final : SpeechHypothesisKind::Partial,
                                            *start,
                                            *end,
                                            TranscriptConfidence(segments),
                                            {text_.data(), size}};
            }

            /** @brief Waits for timed partials, while a final without timing fails deterministically. */
            static SpeechProviderPoll MissingTiming(const bool isFinal) noexcept {
                return isFinal ? SpeechProviderPoll{SpeechProviderInvalidTranscript{}} : SpeechProviderPoll{};
            }

            std::shared_ptr<RecognitionMailbox> mailbox_;
            __strong AVAudioFormat *format_{nil};
            __strong SFSpeechRecognizer *recognizer_{nil};
            __strong SFSpeechAudioBufferRecognitionRequest *request_{nil};
            __strong SFSpeechRecognitionTask *task_{nil};
            std::optional<Audio::AudioMonotonicTimestamp> origin_;
            std::uint64_t framesSubmitted_{};
            std::array<char, ImmersiveSpeechEvidence::MaximumTextBytes> text_{};
            bool sealed_{};
        };

        /** @brief Builds native PCM only for the semantic mono and stereo layouts consumed by Speech. */
        AVAudioFormat *MakeSpeechFormat(const Audio::AudioProcessingFormat &format) {
            const auto channels = format.layout.orderedChannels.size();
            if (channels == 0 || channels > 2 || !Audio::ValidateAudioProcessingFormat(format))
                return nil;
            const auto preset = channels == 1 ? Audio::AudioSpeakerPreset::Mono : Audio::AudioSpeakerPreset::Stereo;
            if (format.layout != Audio::MakeAudioSpeakerLayout(preset))
                return nil;
            return [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                    sampleRate:format.sampleRate
                                                      channels:static_cast<AVAudioChannelCount>(channels)
                                                   interleaved:NO];
        }
    }  // namespace

    /** @copydoc ObserveMacSpeechAuthorization */
    MacSpeechAuthorization ObserveMacSpeechAuthorization() noexcept {
        @autoreleasepool {
            using enum MacSpeechAuthorization;
            if (!HasSpeechUsageDescription())
                return Unavailable;
            switch (SFSpeechRecognizer.authorizationStatus) {
                case SFSpeechRecognizerAuthorizationStatusAuthorized:
                    return Granted;
                case SFSpeechRecognizerAuthorizationStatusNotDetermined:
                    return permissionPending.load() ? Pending : Required;
                case SFSpeechRecognizerAuthorizationStatusDenied:
                case SFSpeechRecognizerAuthorizationStatusRestricted:
                    return Denied;
            }
            return Unavailable;
        }
    }

    /** @copydoc RequestMacSpeechAuthorization */
    MacSpeechAuthorization RequestMacSpeechAuthorization() noexcept {
        @autoreleasepool {
            using enum MacSpeechAuthorization;
            if (!NSThread.isMainThread)
                return Unavailable;
            if (const auto status = ObserveMacSpeechAuthorization(); status != Required)
                return status;
            if (!permissionPending.exchange(true))
                [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus) {
                  permissionPending.store(false);
                }];
            return Pending;
        }
    }

    /** @copydoc MacOnDeviceSpeechProvider::Open */
    std::unique_ptr<ISpeechProviderStream> MacOnDeviceSpeechProvider::Open(const std::string_view locale,
                                                                           const Audio::AudioProcessingFormat &format,
                                                                           const std::stop_token stop) noexcept {
        @autoreleasepool {
            if (stop.stop_requested() || locale.empty() || locale.size() > 35 ||
                ObserveMacSpeechAuthorization() != MacSpeechAuthorization::Granted)
                return {};
            try {
                AVAudioFormat *nativeFormat = MakeSpeechFormat(format);
                NSString *identifier = [[NSString alloc] initWithBytes:locale.data() length:locale.size() encoding:NSUTF8StringEncoding];
                if (!nativeFormat || !identifier)
                    return {};
                NSLocale *language = [NSLocale localeWithLocaleIdentifier:identifier];
                SFSpeechRecognizer *recognizer = [[SFSpeechRecognizer alloc] initWithLocale:language];
                auto stream = std::make_unique<MacSpeechStream>(nativeFormat);
                if (stop.stop_requested() || !stream->Begin(recognizer))
                    return {};
                return stream;
            } catch (const std::bad_alloc &) {
                return {};
            }
        }
    }
}  // namespace Horo::Editor
