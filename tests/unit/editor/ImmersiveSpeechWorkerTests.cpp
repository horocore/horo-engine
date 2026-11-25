#include "../../../apps/HoroEditor/app/ImmersiveSpeechWorker.h"

#include <catch2/catch_test_macros.hpp>
#include <new>

namespace Horo::Editor {
    namespace {
        using enum SpeechTranscriptionStatus;
        namespace Capture = Audio::Backend;

        /** @brief Worker-only fixture checks sample provenance and every sequence it receives. */
        class OrderedSpeechStream final : public ISpeechProviderStream {
        public:
            explicit OrderedSpeechStream(const float confidence) noexcept : confidence_(confidence) {}

            bool Append(const Capture::AudioCapturedBlock &block, const std::span<const Audio::AudioSample> pcm,
                        std::stop_token) noexcept override {
                ++received_;
                return pcm.size() == 1 && block.sequence == received_ && pcm.front() == static_cast<float>(received_) / 4.0F;
            }

            bool FinishInput(std::stop_token) noexcept override {
                finished_ = received_ == 2;
                return finished_;
            }

            SpeechProviderPoll Poll(std::stop_token) noexcept override {
                using enum SpeechHypothesisKind;
                if (finished_)
                    return SpeechHypothesisView{Final, {5, 1'000'000}, {5, 1'040'000}, confidence_, "Both blocks"};
                if (received_ != 0 && !std::exchange(partial_, true))
                    return SpeechHypothesisView{Partial, {5, 1'000'000}, {5, 1'010'000}, 0.9F, "First block"};
                return {};
            }

            void Cancel() noexcept override {
                // The fixture starts no nested work.
            }

        private:
            std::uint64_t received_{};
            float confidence_{};
            bool finished_{};
            bool partial_{};
        };

        class OrderedSpeechProvider final : public ISpeechProvider {
        public:
            explicit OrderedSpeechProvider(const float confidence) noexcept : confidence_(confidence) {}

            std::unique_ptr<ISpeechProviderStream> Open(std::string_view, const Audio::AudioProcessingFormat &,
                                                        std::stop_token) noexcept override {
                try {
                    return std::make_unique<OrderedSpeechStream>(confidence_);
                } catch (const std::bad_alloc &) {
                    return {};
                }
            }

        private:
            float confidence_{};
        };

        struct SpeechWorkerFixture final {
            const Capture::AudioCaptureBufferDescriptor source{.device = {Audio::AudioRuntimeId::Create(2).Value(), 3, 1},
                                                               .generation = 4,
                                                               .clockDomain = 5,
                                                               .format = {48'000,
                                                                          Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Mono)},
                                                               .slots = 2,
                                                               .maximumFrames = 2,
                                                               .budgetBytes = 4096};
            ImmersiveAuthoringContext owner;
            std::stop_source capture;
            ImmersiveSpeechWorker worker;

            void Start(const float confidence = 0.9F) {
                const XR::XRSessionId session{{XR::XRRuntimeGeneration::Create(1).Value(), {0, 1}}, {0, 1}};
                const ImmersiveContextIdentity identity{1,
                                                        1,
                                                        1,
                                                        session,
                                                        {session, {0, 1}},
                                                        1,
                                                        1,
                                                        1,
                                                        XR::XRCapabilityRevision::Create(1).Value(),
                                                        ImmersiveAuthoringMode::Edit,
                                                        0,
                                                        true,
                                                        true};
                const auto context = owner.Publish(identity, {});
                REQUIRE(context);
                REQUIRE(worker.Start(owner, *context, source, capture.get_token(), std::make_shared<OrderedSpeechProvider>(confidence),
                                     "en-US", 0.5F) == Accepted);
            }
        };

        /** @brief Waits for the actual background adapter while keeping the test bounded on failure. */
        SpeechTranscriptionUpdate WaitForUpdate(ImmersiveSpeechWorker &worker) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                auto update = worker.Poll();
                if (update.status != Pending)
                    return update;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Speech worker did not publish an update.");
            return {};
        }

        TEST_CASE("Speech worker drains live PCM before its sealed tail and preserves lease revocation", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            float confidence{};
            SECTION("accepted final stays revocable") {
                confidence = 0.9F;
            }
            SECTION("low confidence final retires the worker") {
                confidence = 0.2F;
            }
            fixture.Start(confidence);
            auto native = Capture::AudioCaptureBuffer::Create(fixture.source);
            REQUIRE(native.HasValue());
            auto tail = std::move(native).Value();
            std::array<Audio::AudioSample, 1> first{0.25F};
            const std::array<Audio::AudioSample, 1> second{0.5F};
            REQUIRE(tail.TryWrite({5, 1'000'000}, 0, 1, first) == Capture::AudioCaptureWrite::Accepted);
            REQUIRE(tail.TryWrite({5, 1'020'833}, 1, 1, second) == Capture::AudioCaptureWrite::Accepted);
            Capture::AudioCapturedBlock block{};
            REQUIRE(tail.TryRead(block, first) == Capture::AudioCaptureRead::Copied);
            REQUIRE(fixture.worker.Submit(block, first) == Accepted);
            first.front() = 0;
            const auto partial = WaitForUpdate(fixture.worker);
            REQUIRE(partial.evidence);
            CHECK(partial.evidence->Hypothesis().text == "First block");
            REQUIRE(fixture.worker.Seal(std::move(tail)) == Accepted);
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK_FALSE(fixture.worker.Busy());
            if (confidence < 0.5F) {
                CHECK(completed.status == LowConfidence);
                CHECK_FALSE(completed.evidence);
                CHECK_FALSE(partial.evidence->IsCurrent());
                return;
            }
            REQUIRE(completed.status == Finished);
            REQUIRE(completed.evidence);
            CHECK(completed.evidence->Hypothesis().text == "Both blocks");
            fixture.capture.request_stop();
            CHECK_FALSE(completed.evidence->IsCurrent());
            CHECK_FALSE(partial.evidence->IsCurrent());
        }

        TEST_CASE("Speech worker cancels when live PCM misses its original source sequence", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            fixture.Start();
            const std::array<Audio::AudioSample, 1> pcm{0.25F};
            const Capture::AudioCapturedBlock skipped{.start = {5, 1'000'000}, .firstFrame = 1, .sequence = 2, .frames = 1};
            CHECK(fixture.worker.Submit(skipped, pcm) == CaptureInterrupted);
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK(completed.status == Cancelled);
            CHECK_FALSE(completed.evidence);
        }
    }  // namespace
}  // namespace Horo::Editor
