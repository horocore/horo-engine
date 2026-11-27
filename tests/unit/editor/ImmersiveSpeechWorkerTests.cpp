#include "../../../apps/HoroEditor/app/ImmersiveSpeechWorker.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <new>

namespace Horo::Editor {
    namespace {
        using enum SpeechTranscriptionStatus;
        namespace Capture = Audio::Backend;

        /** @brief Worker-only fixture checks sample provenance and every sequence it receives. */
        class OrderedSpeechStream final : public ISpeechProviderStream {
        public:
            OrderedSpeechStream(const float confidence, const bool reviseDraft, std::stop_source *const revokeOnFinal,
                                const std::string_view finalText) noexcept
                : revokeOnFinal_(revokeOnFinal), finalText_(finalText), confidence_(confidence), reviseDraft_(reviseDraft) {}

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
                    return SpeechHypothesisView{Final, {5, 1'000'000}, {5, 1'040'000}, confidence_, finalText_};
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
            std::string_view finalText_;
            std::uint64_t received_{};
            float confidence_{};
            bool finished_{};
            bool reviseDraft_{};
            unsigned drafted_{};
        };

        class OrderedSpeechProvider final : public ISpeechProvider {
        public:
            OrderedSpeechProvider(const float confidence, const bool reviseDraft, std::stop_source *const revokeOnFinal,
                                  const std::string_view finalText) noexcept
                : revokeOnFinal_(revokeOnFinal), finalText_(finalText), confidence_(confidence), reviseDraft_(reviseDraft) {}

            std::unique_ptr<ISpeechProviderStream> Open(std::string_view, const Audio::AudioProcessingFormat &,
                                                        std::stop_token) noexcept override {
                try {
                    return std::make_unique<OrderedSpeechStream>(confidence_, reviseDraft_, revokeOnFinal_, finalText_);
                } catch (const std::bad_alloc &) {
                    return {};
                }
            }

        private:
            std::stop_source *revokeOnFinal_{};
            std::string_view finalText_;
            float confidence_{};
            bool reviseDraft_{};
        };

        /** @brief Signals when a failed provider has been released by the worker. */
        class UnavailableSpeechProvider final : public ISpeechProvider {
        public:
            explicit UnavailableSpeechProvider(std::promise<void> &retired) noexcept : retired_(retired) {}

            ~UnavailableSpeechProvider() override {
                retired_.set_value();
            }

            std::unique_ptr<ISpeechProviderStream> Open(std::string_view, const Audio::AudioProcessingFormat &,
                                                        std::stop_token) noexcept override {
                return {};
            }

        private:
            std::promise<void> &retired_;
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

            void StartWith(std::shared_ptr<ISpeechProvider> provider) {
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
                REQUIRE(worker.Start(owner, *context, source, capture.get_token(), std::move(provider), "en-US", 0.5F) == Accepted);
            }

            void Start(const float confidence = 0.9F, const bool reviseDraft = false, const bool revokeOnFinal = false,
                       const std::string_view finalText = "Both blocks") {
                StartWith(std::make_shared<OrderedSpeechProvider>(confidence, reviseDraft, revokeOnFinal ? &capture : nullptr, finalText));
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

        TEST_CASE("Retiring the speech context suppresses an already completed provider failure", "[agent][immersive][speech]") {
            std::promise<void> retired;
            const auto workerReleased = retired.get_future();
            SpeechWorkerFixture fixture;
            fixture.StartWith(std::make_shared<UnavailableSpeechProvider>(retired));
            REQUIRE(workerReleased.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
            auto expected = ProviderUnavailable;
            SECTION("the original context can still report the failure") {}
            SECTION("a retired context only reports its withdrawal") {
                fixture.owner.Revoke();
                expected = StaleContext;
            }
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK(completed.status == expected);
            CHECK_FALSE(completed.evidence);
        }

        TEST_CASE("Speech worker drains live PCM before its sealed tail and preserves lease revocation", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            CHECK_FALSE(fixture.worker.ReadProgress());
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
            const auto started = fixture.worker.ReadProgress();
            REQUIRE(started);
            CHECK(started->admittedAudio == std::chrono::nanoseconds::zero());
            CHECK_FALSE(started->afterCapture);
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
            const auto progressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            auto live = fixture.worker.ReadProgress();
            while (live && live->admittedAudio == std::chrono::nanoseconds::zero() &&
                   std::chrono::steady_clock::now() < progressDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                live = fixture.worker.ReadProgress();
            }
            REQUIRE(live);
            CHECK(live->admittedAudio == std::chrono::nanoseconds{20'833});
            CHECK_FALSE(live->afterCapture);
            const auto partial = WaitForUpdate(fixture.worker);
            REQUIRE(partial.evidence);
            CHECK(partial.evidence->PreviewText(SpeechHypothesisKind::Partial) == "First block");
            CHECK(partial.evidence->PreviewText(SpeechHypothesisKind::Final).empty());
            REQUIRE(fixture.worker.Seal(std::move(tail)) == Accepted);
            const auto finishing = fixture.worker.ReadProgress();
            if (finishing) {
                CHECK(finishing->afterCapture.has_value());
                CHECK(finishing->admittedAudio >= live->admittedAudio);
            }
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK_FALSE(fixture.worker.Busy());
            REQUIRE(completed.status == expected);
            if (expected != Finished) {
                CHECK_FALSE(completed.evidence);
                CHECK_FALSE(fixture.worker.ReadProgress());
                CheckRevokedDraft(partial);
                return;
            }
            REQUIRE(completed.evidence);
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Final) == "Both blocks");
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Partial).empty());
            const auto finalProgress = fixture.worker.ReadProgress();
            REQUIRE(finalProgress);
            REQUIRE(finishing);
            REQUIRE(finalProgress->afterCapture);
            CHECK(finalProgress->afterCapture >= finishing->afterCapture);
            CHECK(finalProgress->admittedAudio == std::chrono::nanoseconds{41'666});
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            REQUIRE(fixture.worker.ReadProgress());
            CHECK(fixture.worker.ReadProgress()->afterCapture == finalProgress->afterCapture);
            CHECK(fixture.worker.OwnsEvidence(*completed.evidence));
            const ImmersiveSpeechWorker foreign;
            CHECK_FALSE(foreign.OwnsEvidence(*completed.evidence));
            fixture.capture.request_stop();
            CHECK_FALSE(fixture.worker.ReadProgress());
            CHECK_FALSE(fixture.worker.OwnsEvidence(*completed.evidence));
            CHECK_FALSE(completed.evidence->IsCurrent());
            CheckRevokedDraft(partial);
            CHECK(completed.evidence->PreviewText(SpeechHypothesisKind::Final).empty());
        }

        TEST_CASE("Capture withdrawal invalidates the displayed draft before the worker reply is polled", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            fixture.Start();
            const std::array<Audio::AudioSample, 1> pcm{0.25F};
            const Capture::AudioCapturedBlock first{.start = {5, 1'000'000}, .firstFrame = 0, .sequence = 1, .frames = 1};
            REQUIRE(fixture.worker.Submit(first, pcm) == Accepted);
            const auto draft = WaitForUpdate(fixture.worker);
            REQUIRE(draft.evidence);
            REQUIRE(draft.evidence->PreviewText(SpeechHypothesisKind::Partial) == "First block");

            fixture.capture.request_stop();
            CheckRevokedDraft(draft);
            CHECK_FALSE(fixture.worker.OwnsEvidence(*draft.evidence));
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK(completed.status == Cancelled);
            CHECK_FALSE(completed.evidence);
        }

        TEST_CASE("Retired authoring context refuses live PCM and the detached tail", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            fixture.Start();
            const std::array<Audio::AudioSample, 1> firstPcm{0.25F};
            const Capture::AudioCapturedBlock first{.start = {5, 1'000'000}, .firstFrame = 0, .sequence = 1, .frames = 1};
            REQUIRE(fixture.worker.Submit(first, firstPcm) == Accepted);
            const auto draft = WaitForUpdate(fixture.worker);
            REQUIRE(draft.evidence);

            const std::array<Audio::AudioSample, 1> secondPcm{0.5F};
            const Capture::AudioCapturedBlock second{.start = {5, 1'020'833}, .firstFrame = 1, .sequence = 2, .frames = 1};
            auto detached = Capture::AudioCaptureBuffer::Create(fixture.source);
            REQUIRE(detached.HasValue());
            auto tail = std::move(detached).Value();
            REQUIRE(tail.TryWrite(second.start, second.firstFrame, second.frames, secondPcm) == Capture::AudioCaptureWrite::Accepted);

            fixture.owner.Revoke();
            REQUIRE_FALSE(fixture.capture.stop_requested());
            CHECK(fixture.worker.Submit(second, secondPcm) == StaleContext);
            CHECK(fixture.worker.Seal(std::move(tail)) == StaleContext);
            CheckRevokedDraft(draft);
            CHECK_FALSE(fixture.worker.ReadProgress());
            const auto completed = WaitForUpdate(fixture.worker);
            CHECK(completed.status == StaleContext);
            CHECK_FALSE(completed.evidence);
        }

        TEST_CASE("Replacing the speech provider retires old evidence and requires fresh sealed capture", "[agent][immersive][speech]") {
            SpeechWorkerFixture fixture;
            const auto finishFreshCapture = [&] {
                const std::array<Audio::AudioSample, 1> firstPcm{0.25F};
                const Capture::AudioCapturedBlock first{.start = {5, 1'000'000}, .firstFrame = 0, .sequence = 1, .frames = 1};
                REQUIRE(fixture.worker.Submit(first, firstPcm) == Accepted);
                const auto draft = WaitForUpdate(fixture.worker);
                REQUIRE(draft.evidence);
                CHECK(draft.evidence->PreviewText(SpeechHypothesisKind::Final).empty());
                const std::array<Audio::AudioSample, 1> secondPcm{0.5F};
                const Capture::AudioCapturedBlock second{.start = {5, 1'020'833}, .firstFrame = 1, .sequence = 2, .frames = 1};
                REQUIRE(fixture.worker.Submit(second, secondPcm) == Accepted);
                auto tail = Capture::AudioCaptureBuffer::Create(fixture.source);
                REQUIRE(tail.HasValue());
                REQUIRE(fixture.worker.Seal(std::move(tail).Value()) == Accepted);
                auto final = WaitForUpdate(fixture.worker);
                REQUIRE(final.status == Finished);
                REQUIRE(final.evidence);
                CHECK(fixture.worker.OwnsEvidence(*final.evidence));
                CheckRevokedDraft(draft);
                return std::move(*final.evidence);
            };

            fixture.Start();
            const auto previous = finishFreshCapture();
            CHECK(previous.PreviewText(SpeechHypothesisKind::Final) == "Both blocks");
            fixture.capture = std::stop_source{};
            fixture.Start(0.9F, false, false, "New provider transcript");
            CHECK_FALSE(fixture.worker.OwnsEvidence(previous));
            CHECK(previous.PreviewText(SpeechHypothesisKind::Final).empty());
            const auto current = finishFreshCapture();
            CHECK(current.PreviewText(SpeechHypothesisKind::Final) == "New provider transcript");
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
            CHECK_FALSE(fixture.worker.OwnsEvidence(*draft.evidence));
            CHECK(draft.evidence->PreviewText(SpeechHypothesisKind::Partial).empty());
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
