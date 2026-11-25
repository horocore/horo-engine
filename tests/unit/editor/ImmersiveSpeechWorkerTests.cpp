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
            OrderedSpeechStream(const float confidence, const bool reviseDraft, std::stop_source *const revokeOnFinal) noexcept
                : revokeOnFinal_(revokeOnFinal), confidence_(confidence), reviseDraft_(reviseDraft) {}

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
                if (finished_) {
                    if (revokeOnFinal_)
                        revokeOnFinal_->request_stop();
                    return SpeechHypothesisView{Final, {5, 1'000'000}, {5, 1'040'000}, confidence_, "Both blocks"};
                }
                if (received_ != 0 && drafted_ == 0) {
                    drafted_ = 1;
                    return SpeechHypothesisView{Partial, {5, 1'000'000}, {5, 1'010'000}, 0.9F, "First block"};
                }
                if (reviseDraft_ && received_ == 2 && drafted_ == 1) {
                    drafted_ = 2;
                    return SpeechHypothesisView{Partial, {5, 1'000'000}, {5, 1'040'000}, 0.2F, "Unsure"};
                }
                return {};
            }

            void Cancel() noexcept override {
                // The fixture starts no nested work.
            }

        private:
            std::stop_source *revokeOnFinal_{};
            std::uint64_t received_{};
            float confidence_{};
            bool finished_{};
            bool reviseDraft_{};
            unsigned drafted_{};
        };

        class OrderedSpeechProvider final : public ISpeechProvider {
        public:
            OrderedSpeechProvider(const float confidence, const bool reviseDraft, std::stop_source *const revokeOnFinal) noexcept
                : revokeOnFinal_(revokeOnFinal), confidence_(confidence), reviseDraft_(reviseDraft) {}

            std::unique_ptr<ISpeechProviderStream> Open(std::string_view, const Audio::AudioProcessingFormat &,
                                                        std::stop_token) noexcept override {
                try {
                    return std::make_unique<OrderedSpeechStream>(confidence_, reviseDraft_, revokeOnFinal_);
                } catch (const std::bad_alloc &) {
                    return {};
                }
            }

        private:
            std::stop_source *revokeOnFinal_{};
            float confidence_{};
            bool reviseDraft_{};
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

            void Start(const float confidence = 0.9F, const bool reviseDraft = false, const bool revokeOnFinal = false) {
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
                auto provider = std::make_shared<OrderedSpeechProvider>(confidence, reviseDraft, revokeOnFinal ? &capture : nullptr);
                REQUIRE(worker.Start(owner, *context, source, capture.get_token(), std::move(provider), "en-US", 0.5F) == Accepted);
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

        /** @brief Checks a previously published draft cannot be read after the speech lease is withdrawn. */
        void CheckRevokedDraft(const SpeechTranscriptionUpdate &draft) {
            REQUIRE(draft.evidence);
            CHECK_FALSE(draft.evidence->IsCurrent());
            CHECK(draft.evidence->PreviewText(SpeechHypothesisKind::Partial).empty());
        }

        TEST_CASE("Speech worker drains live PCM before its sealed tail and preserves lease revocation", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            auto expected = Finished;
            SECTION("accepted final stays revocable") {
                expected = Finished;
            }
            SECTION("low confidence final retires the worker") {
                expected = LowConfidence;
            }
            SECTION("permission revocation during final polling hides both results") {
                expected = Cancelled;
            }
            fixture.Start(expected == LowConfidence ? 0.2F : 0.9F, false, expected == Cancelled);
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
            CHECK(partial.evidence->PreviewText(SpeechHypothesisKind::Partial) == "First block");
            CHECK(partial.evidence->PreviewText(SpeechHypothesisKind::Final).empty());
            REQUIRE(fixture.worker.Seal(std::move(tail)) == Accepted);
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK_FALSE(fixture.worker.Busy());
            REQUIRE(completed.status == expected);
            if (expected != Finished) {
                CHECK_FALSE(completed.evidence);
                CheckRevokedDraft(partial);
                return;
            }
            REQUIRE(completed.evidence);
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Final) == "Both blocks");
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Partial).empty());
            CHECK(fixture.worker.OwnsEvidence(*completed.evidence));
            const ImmersiveSpeechWorker foreign;
            CHECK_FALSE(foreign.OwnsEvidence(*completed.evidence));
            fixture.capture.request_stop();
            CHECK_FALSE(fixture.worker.OwnsEvidence(*completed.evidence));
            CHECK_FALSE(completed.evidence->IsCurrent());
            CheckRevokedDraft(partial);
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Final).empty());
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

        TEST_CASE("Speech worker withdraws a superseded draft and can still finish", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            fixture.Start(0.9F, true);
            const std::array<Audio::AudioSample, 1> firstPcm{0.25F};
            const Capture::AudioCapturedBlock first{.start = {5, 1'000'000}, .firstFrame = 0, .sequence = 1, .frames = 1};
            REQUIRE(fixture.worker.Submit(first, firstPcm) == Accepted);
            const auto draft = WaitForUpdate(fixture.worker);
            REQUIRE(draft.evidence);
            CHECK(draft.evidence->PreviewText(SpeechHypothesisKind::Partial) == "First block");

            const std::array<Audio::AudioSample, 1> secondPcm{0.5F};
            const Capture::AudioCapturedBlock second{.start = {5, 1'020'833}, .firstFrame = 1, .sequence = 2, .frames = 1};
            REQUIRE(fixture.worker.Submit(second, secondPcm) == Accepted);
            const auto withdrawn = WaitForUpdate(fixture.worker);
            CHECK(withdrawn.status == DraftWithdrawn);
            CHECK_FALSE(withdrawn.evidence);
            CHECK(fixture.worker.Busy());

            auto emptyTail = Capture::AudioCaptureBuffer::Create(fixture.source);
            REQUIRE(emptyTail.HasValue());
            REQUIRE(fixture.worker.Seal(std::move(emptyTail).Value()) == Accepted);
            const auto final = WaitForUpdate(fixture.worker);
            REQUIRE(final.status == Finished);
            REQUIRE(final.evidence);
            CHECK(final.evidence->PreviewText(SpeechHypothesisKind::Final) == "Both blocks");
        }
    }  // namespace
}  // namespace Horo::Editor
