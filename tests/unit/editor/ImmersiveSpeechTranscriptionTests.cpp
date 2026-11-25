#include "../../../apps/HoroEditor/app/ImmersiveCommandEnvelope.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Editor {
    namespace {
        using Status = SpeechTranscriptionStatus;
        using Kind = SpeechHypothesisKind;
        namespace Capture = Audio::Backend;

        /** @brief Records only operation counts and supplies controlled provider updates to the test session. */
        struct SpeechFixtureProvider final : ISpeechProvider {
            struct Stream final : ISpeechProviderStream {
                explicit Stream(SpeechFixtureProvider &state) : fixture(state) {}

                bool Append(const Capture::AudioCapturedBlock &, std::span<const Audio::AudioSample>, std::stop_token) noexcept override {
                    ++fixture.blocks;
                    return fixture.available;
                }

                bool FinishInput(std::stop_token) noexcept override {
                    ++fixture.finished;
                    return fixture.available;
                }

                SpeechProviderPoll Poll(std::stop_token) noexcept override {
                    if (fixture.stopDuringPoll)
                        fixture.stopDuringPoll->request_stop();
                    return fixture.next;
                }

                void Cancel() noexcept override {
                    ++fixture.cancelled;
                }

                SpeechFixtureProvider &fixture;
            };

            std::unique_ptr<ISpeechProviderStream> Open(std::string_view locale, const Audio::AudioProcessingFormat &format,
                                                        std::stop_token) noexcept override {
                ++opens;
                CHECK(locale == "tr-TR");
                CHECK(format.sampleRate == 48'000);
                if (!available)
                    return {};
                return std::make_unique<Stream>(*this);
            }

            std::stop_source *stopDuringPoll{};
            SpeechProviderPoll next;
            unsigned opens{};
            unsigned blocks{};
            unsigned finished{};
            unsigned cancelled{};
            bool available{true};
        };

        /** @brief Owns one true authoring generation and one checked microphone source. */
        struct SpeechFixture final {
            ImmersiveAuthoringContext owner;
            const Capture::AudioCaptureBufferDescriptor source{.device = {Audio::AudioRuntimeId::Create(2).Value(), 3, 1},
                                                               .generation = 4,
                                                               .clockDomain = 5,
                                                               .format = {48'000,
                                                                          Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Mono)},
                                                               .slots = 2,
                                                               .maximumFrames = 2,
                                                               .budgetBytes = 4096};
            std::stop_source stop;
            SpeechFixtureProvider provider;
            std::unique_ptr<ImmersiveSpeechTranscription> stream;

            /** @brief Publishes a real context whose live owner can revoke it while the worker holds a copy. */
            ImmersivePlanningContext Context() {
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
                return *context;
            }

            void Prepare() {
                auto prepared = ImmersiveSpeechTranscription::Prepare(owner, Context(), source, "tr-TR", 0.5F, stop.get_token());
                REQUIRE(prepared.status == Status::Accepted);
                REQUIRE(prepared.stream);
                stream = std::move(prepared.stream);
            }

            void StartWithAudio() {
                Prepare();
                REQUIRE(stream->Start(provider) == Status::Accepted);
                REQUIRE(stream->Append(source, FirstBlock(), Samples) == Status::Accepted);
            }

            /** @brief Returns the first sample at a nonzero point on the admitted clock. */
            static Capture::AudioCapturedBlock FirstBlock() {
                return {.start = {5, 1'000'000}, .sequence = 1, .frames = 2};
            }

            static constexpr std::array<Audio::AudioSample, 2> Samples{0.125F, -0.125F};
        };

        /** @brief Uses a tight provider window inside the admitted PCM. */
        SpeechHypothesisView Transcript(const Kind kind = Kind::Partial) {
            return {kind, {5, 1'000'000}, {5, 1'040'000}, 0.8F, "Seçili nesne"};
        }

        /** @brief Makes one owner-scoped XR observation with an independently measured audio-clock interval. */
        ImmersiveSpatialObservation SpatialObservation(const ImmersivePlanningContext &context, const std::uint64_t revision,
                                                       const std::uint64_t earliest, const std::uint64_t latest) {
            const auto session = context.Identity().xrSession;
            const auto origin = XR::XRWorldOriginRevision::Create(1).Value();
            return {{session, XR::XRTrackingSnapshotRevision::Create(revision).Value(), origin,
                     XR::XRRuntimeTime::Create(static_cast<std::int64_t>(revision) * 17).Value(), 2},
                    {{session, {1, 2}},
                     XR::XRControllerInteractionPose::Aim,
                     {context.Identity().referenceSpace, XR::XRSpaceKind::World, origin},
                     XR::XRTrackingConfidence::High,
                     Math::Vec3{}},
                    {5, earliest},
                    {5, latest}};
        }

        /** @brief Produces final evidence from the actual provider and transcription lifecycle. */
        ImmersiveSpeechEvidence FinalEvidence(SpeechFixture &fixture) {
            fixture.StartWithAudio();
            REQUIRE(fixture.stream->FinishInput() == Status::Accepted);
            fixture.provider.next = Transcript(Kind::Final);
            auto result = fixture.stream->Poll();
            REQUIRE(result.status == Status::Finished);
            REQUIRE(result.evidence);
            return std::move(*result.evidence);
        }

        TEST_CASE("Immersive commands retain only spatial evidence aligned with final speech", "[agent][immersive][speech]") {
            using enum ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            std::array samples{SpatialObservation(context, 1, 970'000, 990'000), SpatialObservation(context, 2, 1'005'000, 1'010'000),
                               SpatialObservation(context, 3, 1'020'000, 1'030'000), SpatialObservation(context, 4, 1'050'000, 1'060'000)};
            const auto cutoff = samples.back().cutoff;
            const auto assembled =
                ImmersiveCommandEnvelope::Assemble(context, speech, ImmersiveSpatialHistoryView{context, cutoff, samples});
            REQUIRE(assembled.status == Aligned);
            REQUIRE(assembled.envelope);
            samples[1].earliest.nanoseconds = 0;
            REQUIRE(assembled.envelope->Spatial().size() == 2);
            CHECK(assembled.envelope->Spatial().front().earliest.nanoseconds == 1'005'000);
            CHECK(assembled.envelope->Spatial().back().cutoff.tracking.Value() == 3);
            CHECK(assembled.envelope->Speech().Hypothesis().text == "Seçili nesne");
            CHECK(assembled.envelope->IsCurrent(cutoff));
            auto rebound = cutoff;
            ++rebound.bindingRevision;
            CHECK_FALSE(assembled.envelope->IsCurrent(rebound));
            const auto speechOnly = ImmersiveCommandEnvelope::Assemble(context, speech);
            REQUIRE(speechOnly.status == SpeechOnly);
            REQUIRE(speechOnly.envelope);
            CHECK(speechOnly.envelope->Spatial().empty());
            CHECK(speechOnly.envelope->IsCurrent());
            fixture.stop.request_stop();
            CHECK_FALSE(assembled.envelope->IsCurrent(cutoff));
            CHECK_FALSE(speechOnly.envelope->IsCurrent());
        }

        TEST_CASE("Immersive commands reject uncertain timing and mismatched provenance", "[agent][immersive][speech]") {
            using enum ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            std::array samples{SpatialObservation(context, 1, 1'005'000, 1'015'000), SpatialObservation(context, 2, 1'020'000, 1'030'000)};
            auto history = ImmersiveSpatialHistoryView{context, samples.back().cutoff, samples};
            auto expected = AmbiguousTiming;
            SECTION("measurement crosses the phrase boundary") {
                samples[0].earliest.nanoseconds = 990'000;
            }
            SECTION("measurement may land exactly at the exclusive phrase end") {
                samples.back().latest.nanoseconds = Transcript().end.nanoseconds;
            }
            SECTION("two measurement intervals overlap") {
                samples[0].latest.nanoseconds = samples[1].earliest.nanoseconds;
            }
            SECTION("XR sequence disagrees with capture time") {
                std::swap(samples[0].cutoff, samples[1].cutoff);
            }
            SECTION("sample belongs to another clock domain") {
                ++samples[0].latest.clockDomain;
                expected = InvalidSpatialEvidence;
            }
            SECTION("XR origin changed") {
                history.cutoff.origin = XR::XRWorldOriginRevision::Create(2).Value();
                expected = StaleEvidence;
            }
            SECTION("XR session changed") {
                ++history.cutoff.session.slot.generation;
                expected = StaleEvidence;
            }
            const auto result = ImmersiveCommandEnvelope::Assemble(context, speech, history);
            CHECK(result.status == expected);
            CHECK_FALSE(result.envelope);
        }

        TEST_CASE("Immersive commands can retain multiple candidates from a single measured cutoff", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            const auto sample = SpatialObservation(context, 1, 1'005'000, 1'015'000);
            const auto outside = SpatialObservation(context, 2, Transcript().end.nanoseconds, Transcript().end.nanoseconds);
            std::array samples{sample, sample, outside};
            ++samples[1].candidate.device.slot.index;
            const auto result =
                ImmersiveCommandEnvelope::Assemble(context, speech, ImmersiveSpatialHistoryView{context, outside.cutoff, samples});
            REQUIRE(result.status == ImmersiveCommandStatus::Aligned);
            REQUIRE(result.envelope);
            REQUIRE(result.envelope->Spatial().size() == 2);
            CHECK(result.envelope->Spatial()[1].candidate.device != result.envelope->Spatial()[0].candidate.device);
        }

        TEST_CASE("Immersive commands fence history ownership, partial speech and maximum observations", "[agent][immersive][speech]") {
            using enum ImmersiveCommandStatus;
            SpeechFixture fixture;
            fixture.StartWithAudio();
            fixture.provider.next = Transcript();
            const auto partial = fixture.stream->Poll();
            REQUIRE(partial.evidence);
            const auto context = fixture.Context();
            CHECK(ImmersiveCommandEnvelope::Assemble(context, *partial.evidence).status == PartialSpeech);

            SpeechFixture finalFixture;
            const auto speech = FinalEvidence(finalFixture);
            const auto finalContext = finalFixture.Context();
            const auto sample = SpatialObservation(finalContext, 1, 1'005'000, 1'015'000);
            CHECK(
                ImmersiveCommandEnvelope::Assemble(finalContext, speech, ImmersiveSpatialHistoryView{context, sample.cutoff, {&sample, 1}})
                    .status == StaleEvidence);
            const std::array<ImmersiveSpatialObservation, ImmersiveCommandEnvelope::MaximumObservations + 1> oversized{};
            CHECK(ImmersiveCommandEnvelope::Assemble(finalContext, speech,
                                                     ImmersiveSpatialHistoryView{finalContext, sample.cutoff, oversized})
                      .status == CapacityExceeded);
        }

        TEST_CASE("Immersive commands distinguish a measured pose from the observed press range", "[agent][immersive][speech]") {
            using enum ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            auto sample = SpatialObservation(context, 1, 1'010'000, 1'015'000);
            sample.controllerPress = ImmersiveControllerPress{{5, 1'005'000}, {5, 1'025'000}};
            auto expected = Aligned;
            SECTION("both independently measured ranges stay inside the phrase") {}
            SECTION("a pose inside the phrase cannot disambiguate the earlier press") {
                sample.controllerPress->earliest.nanoseconds = 995'000;
                expected = AmbiguousTiming;
            }
            SECTION("a pose inside the phrase cannot disambiguate the later press") {
                sample.controllerPress->latest.nanoseconds = Transcript().end.nanoseconds;
                expected = AmbiguousTiming;
            }
            SECTION("a press from a different clock cannot be combined") {
                ++sample.controllerPress->latest.clockDomain;
                expected = InvalidSpatialEvidence;
            }
            const auto assembled =
                ImmersiveCommandEnvelope::Assemble(context, speech, ImmersiveSpatialHistoryView{context, sample.cutoff, {&sample, 1}});
            CHECK(assembled.status == expected);
            if (expected == Aligned) {
                REQUIRE(assembled.envelope);
                const auto &press = assembled.envelope->Spatial().front().controllerPress;
                REQUIRE(press);
                CHECK(press->earliest.nanoseconds == 1'005'000);
                CHECK(press->latest.nanoseconds == 1'025'000);
            } else {
                CHECK_FALSE(assembled.envelope);
            }
        }

        TEST_CASE("Immersive listening freezes one context's spatial history at seal", "[agent][immersive][speech]") {
            using RecordStatus = ImmersiveSpatialCaptureStatus;
            using CommandStatus = ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            ImmersiveSpatialCapture capture;
            const auto sample = SpatialObservation(context, 1, 1'005'000, 1'015'000);
            const std::span observation{&sample, 1};
            CHECK(capture.Record(context, observation) == RecordStatus::Closed);
            capture.Begin(context);
            CHECK(capture.Assemble(speech, sample.cutoff).status == CommandStatus::SpatialCaptureOpen);
            SpeechFixture foreign;
            CHECK(capture.Record(foreign.Context(), observation) == RecordStatus::StaleContext);
            auto foreignPointer = sample;
            ++foreignPointer.cutoff.session.slot.generation;
            CHECK(capture.RecordPointing(foreignPointer) == RecordStatus::StaleContext);
            REQUIRE(capture.RecordPointing(sample) == RecordStatus::Recorded);
            capture.Seal();
            CHECK(capture.RecordPointing(sample) == RecordStatus::Closed);
            const auto command = capture.Assemble(speech, sample.cutoff);
            REQUIRE(command.status == CommandStatus::Aligned);
            REQUIRE(command.envelope);
            REQUIRE(command.envelope->Spatial().size() == 1);
            CHECK(command.envelope->Spatial().front().cutoff == sample.cutoff);
            fixture.owner.Revoke();
            CHECK(capture.Assemble(speech, sample.cutoff).status == CommandStatus::StaleEvidence);
        }

        TEST_CASE("Immersive listening never assembles a truncated spatial history", "[agent][immersive][speech]") {
            using RecordStatus = ImmersiveSpatialCaptureStatus;
            using CommandStatus = ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            ImmersiveSpatialCapture capture;
            capture.Begin(context);
            std::array<ImmersiveSpatialObservation, ImmersiveCommandEnvelope::MaximumObservations> samples;
            for (std::size_t index = 0; index < samples.size(); ++index) {
                const auto timestamp = 1'000'000 + index * 1'000;
                samples[index] = SpatialObservation(context, index + 1, timestamp, timestamp);
            }
            CHECK(capture.Record(context, samples) == RecordStatus::Recorded);
            CHECK(capture.Record(context, std::span{samples}.first(1)) == RecordStatus::CapacityExceeded);
            capture.Seal();
            const auto overflow = capture.Assemble(speech, samples.back().cutoff);
            CHECK(overflow.status == CommandStatus::CapacityExceeded);
            CHECK_FALSE(overflow.envelope);

            capture.Reset();
            CHECK(capture.Assemble(speech, samples.back().cutoff).status == CommandStatus::StaleEvidence);
            capture.Begin(context);
            capture.Seal();
            const auto restarted = capture.Assemble(speech, {});
            REQUIRE(restarted.status == CommandStatus::SpeechOnly);
            REQUIRE(restarted.envelope);
            CHECK(restarted.envelope->Spatial().empty());
        }

        TEST_CASE("Immersive speech issues owned partial and final evidence after sealing PCM", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            CHECK(fixture.stream->Poll().status == Status::Pending);
            std::string recognizerText{"Seçili nesne"};
            auto transcript = Transcript();
            transcript.text = recognizerText;
            fixture.provider.next = transcript;
            const auto partial = fixture.stream->Poll();
            REQUIRE(partial.evidence);
            recognizerText.front() = 'X';
            CHECK(partial.evidence->Hypothesis().text == "Seçili nesne");
            CHECK(partial.evidence->Locale() == "tr-TR");
            CHECK(partial.evidence->Context().Generation() == fixture.Context().Generation());

            REQUIRE(fixture.stream->FinishInput() == Status::Accepted);
            REQUIRE(fixture.stream->FinishInput() == Status::Accepted);
            CHECK(fixture.provider.finished == 1);
            fixture.provider.next = Transcript(Kind::Final);
            const auto completed = fixture.stream->Poll();
            REQUIRE(completed.status == Status::Finished);
            REQUIRE(completed.evidence);
            CHECK(completed.evidence->Hypothesis().kind == Kind::Final);
            CHECK_FALSE(fixture.stream->Poll().evidence);
            fixture.stream.reset();
            CHECK(completed.evidence->IsCurrent());
            fixture.owner.Revoke();
            CHECK_FALSE(completed.evidence->IsCurrent());
        }

        TEST_CASE("Immersive speech ends on a changed capture or revoked context", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            fixture.provider.next = Transcript();
            const auto partial = fixture.stream->Poll();
            REQUIRE(partial.evidence);
            auto expected = Status::CaptureInterrupted;
            auto source = fixture.source;
            auto block = Capture::AudioCapturedBlock{.start = {5, 1'041'666}, .firstFrame = 2, .sequence = 2, .frames = 2};
            SECTION("device generation changed") {
                ++source.generation;
            }
            SECTION("source discontinuity") {
                block.missingSourceFramesBefore = 1;
            }
            SECTION("ring dropped PCM") {
                block.droppedFramesBefore = 2;
            }
            SECTION("editor context revoked") {
                fixture.owner.Revoke();
                expected = Status::StaleContext;
            }
            SECTION("host muted or withdrew capture permission") {
                fixture.stop.request_stop();
                expected = Status::Cancelled;
            }
            CHECK(fixture.stream->Append(source, block, fixture.Samples) == expected);
            CHECK(fixture.provider.blocks == 1);
            CHECK(fixture.provider.cancelled == 1);
            CHECK_FALSE(partial.evidence->IsCurrent());
        }

        TEST_CASE("Immersive speech rejects stale, malformed and low confidence updates", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            auto transcript = Transcript();
            auto expected = Status::InvalidTranscript;
            const std::string oversized(ImmersiveSpeechEvidence::MaximumTextBytes + 1, 'x');
            SECTION("future transcript") {
                transcript.end.nanoseconds += 100'000;
            }
            SECTION("foreign clock") {
                ++transcript.start.clockDomain;
            }
            SECTION("invalid UTF-8") {
                transcript.text = "\xff";
            }
            SECTION("text exceeds budget") {
                transcript.text = oversized;
            }
            SECTION("unsealed final") {
                transcript.kind = Kind::Final;
            }
            SECTION("missing confidence") {
                transcript.confidence = std::numeric_limits<float>::quiet_NaN();
            }
            SECTION("low confidence") {
                transcript.confidence = 0.2F;
                expected = Status::LowConfidence;
            }
            SECTION("revoked while provider was polling") {
                fixture.provider.stopDuringPoll = &fixture.stop;
                expected = Status::Cancelled;
            }
            fixture.provider.next = transcript;
            const auto update = fixture.stream->Poll();
            CHECK(update.status == expected);
            CHECK_FALSE(update.evidence);
        }

        TEST_CASE("Immersive speech distinguishes adapter-reported malformed output from unavailability", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            fixture.provider.next = SpeechProviderInvalidTranscript{};
            const auto update = fixture.stream->Poll();
            CHECK(update.status == Status::InvalidTranscript);
            CHECK_FALSE(update.evidence);
            CHECK(fixture.provider.cancelled == 1);
        }

        TEST_CASE("Immersive speech bounds provider attempts, hypotheses and audio time", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            SECTION("provider unavailable") {
                fixture.Prepare();
                fixture.provider.available = false;
                CHECK(fixture.stream->Start(fixture.provider) == Status::ProviderUnavailable);
                CHECK(fixture.stream->Start(fixture.provider) == Status::ProviderUnavailable);
                CHECK(fixture.provider.opens == 1);
            }
            SECTION("update flood") {
                fixture.StartWithAudio();
                fixture.provider.next = Transcript();
                for (unsigned update = 0; update < 64; ++update)
                    REQUIRE(fixture.stream->Poll().evidence.has_value());
                CHECK(fixture.stream->Poll().status == Status::LimitExceeded);
            }
            SECTION("PCM window exceeded") {
                fixture.StartWithAudio();
                const Capture::AudioCapturedBlock late{.start = {5, 16'000'000'000}, .firstFrame = 2, .sequence = 2, .frames = 2};
                CHECK(fixture.stream->Append(fixture.source, late, fixture.Samples) == Status::LimitExceeded);
                CHECK(fixture.provider.blocks == 1);
            }
        }
    }  // namespace
}  // namespace Horo::Editor
