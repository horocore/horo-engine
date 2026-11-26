#include "../../../apps/HoroEditor/app/ImmersiveCommandEnvelope.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>

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

        /** @brief Models a cooperative provider blocked while opening its recognizer. */
        struct WaitingSpeechProvider final : ISpeechProvider {
            std::unique_ptr<ISpeechProviderStream> Open(std::string_view, const Audio::AudioProcessingFormat &,
                                                        const std::stop_token stop) noexcept override {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, std::chrono::seconds(1), [] {
                    return false;
                });
                observedStop = stop.stop_requested();
                return {};
            }

            bool observedStop{};
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

        TEST_CASE("Immersive provider deadline requests stop while a provider call is blocked", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            const auto context = fixture.Context();
            WaitingSpeechProvider provider;
            auto prepared = ImmersiveSpeechTranscription::Prepare(fixture.owner, context, fixture.source, "tr-TR", 0.5F,
                                                                  fixture.stop.get_token(), std::chrono::milliseconds(20));
            REQUIRE(prepared.stream);
            CHECK(prepared.stream->Start(provider) == Status::ProviderTimedOut);
            CHECK(provider.observedStop);
            CHECK_FALSE(fixture.stop.stop_requested());
            const auto expired = prepared.stream->Poll();
            CHECK(expired.status == Status::ProviderTimedOut);
            CHECK_FALSE(expired.evidence);
        }

        TEST_CASE("Immersive evidence clears the transcript bytes it owns on release", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            const auto final = FinalEvidence(fixture);
            alignas(ImmersiveSpeechEvidence) std::array<std::byte, sizeof(ImmersiveSpeechEvidence)> memory{};
            auto *const copy = std::construct_at(reinterpret_cast<ImmersiveSpeechEvidence *>(memory.data()), final);
            const auto text = copy->PreviewText(Kind::Final);
            const auto offset = reinterpret_cast<std::uintptr_t>(text.data()) - reinterpret_cast<std::uintptr_t>(memory.data());
            const auto length = text.size();
            const bool exactCopy = text == final.PreviewText(Kind::Final);
            std::destroy_at(copy);
            REQUIRE(length > 0);
            REQUIRE(exactCopy);
            REQUIRE(offset <= memory.size() - length);
            CHECK(std::ranges::all_of(std::span{memory}.subspan(offset, length), [](const std::byte value) {
                return value == std::byte{};
            }));
            CHECK(final.PreviewText(Kind::Final) == Transcript().text);
        }

        TEST_CASE("Immersive speech can produce a local Play diagnostics preview without model authority", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            const auto edit = fixture.Context();
            auto identity = edit.Identity();
            identity.mode = ImmersiveAuthoringMode::Play;
            identity.worldGeneration = 1;
            identity.user = 0;
            identity.conversation = 0;
            identity.toolContext = 0;
            identity.planningAllowed = false;
            identity.localSpeechOnly = true;
            const auto context = fixture.owner.Publish(identity, {});
            REQUIRE(context);
            auto capture =
                ImmersiveSpeechTranscription::Prepare(fixture.owner, *context, fixture.source, "tr-TR", 0.5F, fixture.stop.get_token());
            REQUIRE(capture.status == Status::Accepted);
            REQUIRE(capture.stream->Start(fixture.provider) == Status::Accepted);
            REQUIRE(capture.stream->Append(fixture.source, fixture.FirstBlock(), fixture.Samples) == Status::Accepted);
            REQUIRE(capture.stream->FinishInput() == Status::Accepted);
            fixture.provider.next = Transcript(Kind::Final);
            const auto update = capture.stream->Poll();
            REQUIRE(update.status == Status::Finished);
            REQUIRE(update.evidence);
            const auto assembled = ImmersiveCommandEnvelope::Assemble(*context, *update.evidence);
            CHECK(assembled.status == ImmersiveCommandStatus::SpeechOnly);
            REQUIRE(assembled.envelope);
            CHECK(assembled.envelope->Speech().Hypothesis().text == "Seçili nesne");
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

        TEST_CASE("World UI presses do not become scene evidence while held or after a source replacement", "[agent][immersive][speech]") {
            ImmersivePointingPressEdge edge;
            CHECK_FALSE(edge.Observe(8, false, false));
            CHECK_FALSE(edge.Observe(8, true, true));
            CHECK_FALSE(edge.Observe(8, true, false));
            CHECK_FALSE(edge.Observe(8, false, false));
            CHECK(edge.Observe(8, true, false));
            CHECK_FALSE(edge.Observe(8, true, false));
            edge.Reset();
            CHECK_FALSE(edge.Observe(8, true, false));
            CHECK_FALSE(edge.Observe(9, true, false));
            CHECK_FALSE(edge.Observe(9, false, false));
            CHECK(edge.Observe(9, true, false));
        }

        TEST_CASE("Pointing review preserves distinct locations on the same object", "[agent][immersive][speech]") {
            constexpr std::uint64_t World = 9;
            ImmersiveSpatialObservation first{.controllerPress = ImmersiveControllerPress{},
                                              .pointing =
                                                  ImmersivePointingHits{.worldGeneration = World, .structuralRevision = 1, .count = 1}};
            first.candidate.shape = Math::Ray{};
            first.pointing->hits[0] = {{17}, 2, {1, 2, 3}};
            auto second = first;
            second.pointing->hits[0].worldPosition = {1, 2, 4};
            std::array observations{first, second, first};

            using enum ImmersivePointingReviewKind;
            const auto objects = CollectImmersivePointingHits(observations, World, Objects);
            REQUIRE(objects);
            CHECK(objects->count == 1);
            const auto locations = CollectImmersivePointingHits(observations, World, Locations);
            REQUIRE(locations);
            REQUIRE(locations->count == 2);
            CHECK(locations->hits[0].worldPosition == first.pointing->hits[0].worldPosition);
            CHECK(locations->hits[1].worldPosition == second.pointing->hits[0].worldPosition);
            ++observations[1].pointing->worldGeneration;
            CHECK_FALSE(CollectImmersivePointingHits(observations, World, Locations));
            observations[1].pointing->worldGeneration = World;
            observations[1].pointing->structuralRevision = 2;
            CHECK_FALSE(CollectImmersivePointingHits(observations, World, Locations));
            for (auto &observation : observations)
                observation.pointing->structuralRevision = 2;
            observations[1].pointing->mounted = true;
            CHECK_FALSE(CollectImmersivePointingHits(observations, World, Locations));
            for (auto &observation : observations)
                observation.pointing->mounted = true;
            const auto mounted = CollectImmersivePointingHits(observations, World, Locations);
            REQUIRE(mounted);
            CHECK(mounted->structuralRevision == 2);
            CHECK(mounted->mounted);
        }

        TEST_CASE("Pointing review rejects malformed evidence even from a repeated object", "[agent][immersive][speech]") {
            constexpr std::uint64_t World = 9;
            constexpr float Invalid = std::numeric_limits<float>::quiet_NaN();
            using Hit = ImmersivePointingHits::Hit;
            ImmersiveSpatialObservation valid{.controllerPress = ImmersiveControllerPress{},
                                              .pointing = ImmersivePointingHits{.worldGeneration = World,
                                                                               .structuralRevision = 1,
                                                                               .hits = {Hit{{17}, 2, {1, 2, 3}}},
                                                                               .count = 1}};
            valid.candidate.shape = Math::Ray{};
            auto observations = std::array{valid, valid};
            const auto review = [&] { return CollectImmersivePointingHits(observations, World, ImmersivePointingReviewKind::Objects); };
            REQUIRE(review());
            for (const auto &hit : {Hit{{}, 2, {1, 2, 3}}, Hit{{17}, -1, {1, 2, 3}}, Hit{{17}, Invalid, {1, 2, 3}},
                                    Hit{{17}, 2, {1, Invalid, 3}}}) {
                observations[1].pointing->hits[0] = hit;
                CHECK_FALSE(review());
            }
        }

        TEST_CASE("Physical surface review requires its original grant and current plane classification", "[agent][immersive][speech]") {
            constexpr std::uint64_t World = 7;
            SpeechFixture fixture;
            const auto context = fixture.Context();
            const auto &identity = context.Identity();
            const XR::XRPlaneScope scope{identity.xrSession,
                                         {identity.referenceSpace, XR::XRSpaceKind::World, XR::XRWorldOriginRevision::Create(1).Value()},
                                         XR::XRPlaneGrant::Create(1).Value()};
            XR::XRPlanePublication publication;
            const XR::XRPlaneLimits limits{2, Duration::FromNanoseconds(1), Duration::FromNanoseconds(100)};
            REQUIRE(publication.Activate(scope, limits).HasValue());
            const auto now = XR::XRRuntimeTime::Create(100).Value();
            const XR::XRPlaneObservation wall{.id = {scope.session, {1, 1}},
                                              .extentMeters = {2, 3},
                                              .classification = XR::XRPlaneClassification::Wall,
                                              .confidence = XR::XRTrackingConfidence::High};
            REQUIRE(publication.Publish(scope, now, std::span{&wall, 1}).HasValue());
            const auto live = publication.Read(scope, now);
            REQUIRE(live.HasValue());
            ImmersiveSpatialObservation press{.controllerPress = ImmersiveControllerPress{},
                                              .surfaces = ImmersiveSurfaceHits{.scope = scope, .worldGeneration = World, .count = 1}};
            press.candidate.shape = Math::Ray{};
            press.surfaces->hits[0] = {wall.id, wall.classification, {1, 2, 3}};
            const std::array repeated{press, press};
            const auto candidates = CollectImmersiveSurfaceHits(repeated, World, live.Value());
            REQUIRE(candidates);
            REQUIRE(candidates->count == 1);
            CHECK(candidates->hits[0].classification == wall.classification);
            CHECK_FALSE(CollectImmersiveSurfaceHits(repeated, World + 1, live.Value()));

            auto malformed = repeated;
            auto &hit = malformed[1].surfaces->hits[0];
            hit.worldPosition.x = std::numeric_limits<float>::infinity();
            CHECK_FALSE(CollectImmersiveSurfaceHits(malformed, World, live.Value()));
            hit = press.surfaces->hits[0];
            hit.classification = XR::XRPlaneClassification::Count;
            CHECK_FALSE(CollectImmersiveSurfaceHits(malformed, World, live.Value()));

            const auto correctedAt = XR::XRRuntimeTime::Create(101).Value();
            auto floor = wall;
            floor.classification = XR::XRPlaneClassification::Floor;
            REQUIRE(publication.Publish(scope, correctedAt, std::span{&floor, 1}).HasValue());
            const auto corrected = publication.Read(scope, correctedAt);
            REQUIRE(corrected.HasValue());
            CHECK_FALSE(CollectImmersiveSurfaceHits(repeated, World, corrected.Value()));
            auto fresh = press;
            fresh.surfaces->hits[0].classification = floor.classification;
            const auto freshReview = CollectImmersiveSurfaceHits(std::span{&fresh, 1}, World, corrected.Value());
            REQUIRE(freshReview);
            CHECK(freshReview->hits[0].classification == floor.classification);

            const auto later = XR::XRRuntimeTime::Create(102).Value();
            REQUIRE(publication.Publish(scope, later, {}).HasValue());
            const auto missing = publication.Read(scope, later);
            REQUIRE(missing.HasValue());
            CHECK_FALSE(CollectImmersiveSurfaceHits(repeated, World, missing.Value()));
            publication.Revoke();
            auto replacement = scope;
            replacement.grant = XR::XRPlaneGrant::Create(2).Value();
            REQUIRE(publication.Activate(replacement, limits).HasValue());
            REQUIRE(publication.Publish(replacement, later, std::span{&wall, 1}).HasValue());
            const auto regranted = publication.Read(replacement, later);
            REQUIRE(regranted.HasValue());
            CHECK_FALSE(CollectImmersiveSurfaceHits(repeated, World, regranted.Value()));
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

        TEST_CASE("Immersive commands reject geometry outside the measured World space", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            auto sample = SpatialObservation(context, 1, 1'005'000, 1'015'000);
            SECTION("local reference space uses the same session and origin") {
                sample.candidate.space.kind = XR::XRSpaceKind::Local;
            }
            SECTION("nonfinite direct point") {
                sample.candidate.shape = Math::Vec3{std::numeric_limits<float>::infinity(), 0, 0};
            }
            SECTION("nonfinite proximity radius") {
                sample.candidate.shape = Math::BoundingSphere{{}, std::numeric_limits<float>::infinity()};
            }
            SECTION("degenerate ray direction") {
                sample.candidate.shape = Math::Ray{{}, {}, 0, 4};
            }
            SECTION("empty ray segment") {
                sample.candidate.shape = Math::Ray{{}, {0, 0, -1}, 0, 0};
            }
            const auto result =
                ImmersiveCommandEnvelope::Assemble(context, speech, ImmersiveSpatialHistoryView{context, sample.cutoff, {&sample, 1}});
            CHECK(result.status == ImmersiveCommandStatus::InvalidSpatialEvidence);
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
            SECTION("the action transition can be separate from the pose measurement") {
                sample.controllerPress->earliest.nanoseconds = 1'016'000;
            }
            SECTION("the pose alone cannot align a press outside the phrase") {
                sample.controllerPress = ImmersiveControllerPress{Transcript().end, Transcript().end};
                expected = SpeechOnly;
            }
            SECTION("the press alone cannot align an earlier pose outside the phrase") {
                sample.earliest.nanoseconds = 990'000;
                sample.latest.nanoseconds = 995'000;
                expected = SpeechOnly;
            }
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
            SECTION("an inverted press interval cannot be combined") {
                sample.controllerPress->earliest.nanoseconds = 1'026'000;
                expected = InvalidSpatialEvidence;
            }
            const auto assembled =
                ImmersiveCommandEnvelope::Assemble(context, speech, ImmersiveSpatialHistoryView{context, sample.cutoff, {&sample, 1}});
            CHECK(assembled.status == expected);
            REQUIRE(assembled.envelope.has_value() == (expected == Aligned || expected == SpeechOnly));
            if (!assembled.envelope)
                return;
            const auto spatial = assembled.envelope->Spatial();
            REQUIRE(spatial.size() == (expected == Aligned ? 1U : 0U));
            if (spatial.empty())
                return;
            REQUIRE(spatial.front().controllerPress);
            CHECK(spatial.front().controllerPress->earliest.nanoseconds == sample.controllerPress->earliest.nanoseconds);
            CHECK(spatial.front().controllerPress->latest.nanoseconds == sample.controllerPress->latest.nanoseconds);
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
            const auto preview = capture.AssemblePreview(speech, samples.back().cutoff);
            REQUIRE(preview.status == CommandStatus::SpeechOnlySpatialRejected);
            REQUIRE(preview.envelope);
            CHECK(preview.envelope->Spatial().empty());

            capture.Reset();
            CHECK(capture.Assemble(speech, samples.back().cutoff).status == CommandStatus::StaleEvidence);
            capture.Begin(context);
            capture.Seal();
            const auto restarted = capture.Assemble(speech, {});
            REQUIRE(restarted.status == CommandStatus::SpeechOnly);
            REQUIRE(restarted.envelope);
            CHECK(restarted.envelope->Spatial().empty());
        }

        TEST_CASE("Immersive local preview keeps speech only when optional XR evidence is rejected", "[agent][immersive][speech]") {
            using enum ImmersiveCommandStatus;
            SpeechFixture fixture;
            const auto speech = FinalEvidence(fixture);
            const auto context = fixture.Context();
            auto sample = SpatialObservation(context, 1, 995'000, 1'005'000);
            SECTION("measurement straddles the phrase start") {}
            SECTION("measurement belongs to a different clock domain") {
                ++sample.latest.clockDomain;
            }
            ImmersiveSpatialCapture capture;
            capture.Begin(context);
            REQUIRE(capture.RecordPointing(sample) == ImmersiveSpatialCaptureStatus::Recorded);
            CHECK(capture.AssemblePreview(speech, sample.cutoff).status == SpatialCaptureOpen);
            capture.Seal();
            const auto preview = capture.AssemblePreview(speech, sample.cutoff);
            REQUIRE(preview.status == SpeechOnlySpatialRejected);
            REQUIRE(preview.envelope);
            CHECK(preview.envelope->Speech().Hypothesis().text == Transcript().text);
            CHECK(preview.envelope->Spatial().empty());

            auto foreign = sample.cutoff;
            ++foreign.session.slot.generation;
            CHECK_FALSE(capture.AssemblePreview(speech, foreign).envelope);
            fixture.owner.Revoke();
            CHECK_FALSE(preview.envelope->IsCurrent(sample.cutoff));
            CHECK_FALSE(capture.AssemblePreview(speech, sample.cutoff).envelope);
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

        TEST_CASE("Immersive speech admits capture rounding and rejects overlapping PCM", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            fixture.provider.next = Transcript();
            const auto partial = fixture.stream->Poll();
            REQUIRE(partial.evidence);
            auto block = Capture::AudioCapturedBlock{.start = {5, 1'041'666}, .firstFrame = 2, .sequence = 2, .frames = 2};
            auto expected = Status::Accepted;
            SECTION("next block starts at the floor of the previous frame boundary") {}
            SECTION("next block starts before the rounded boundary") {
                --block.start.nanoseconds;
                expected = Status::CaptureInterrupted;
            }
            CHECK(fixture.stream->Append(fixture.source, block, fixture.Samples) == expected);
            CHECK(fixture.provider.blocks == (expected == Status::Accepted ? 2 : 1));
            CHECK(partial.evidence->IsCurrent() == (expected == Status::Accepted));
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
            SECTION("low confidence partial") {
                transcript.confidence = 0.2F;
                expected = Status::DraftWithdrawn;
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
                const auto exhausted = fixture.stream->Poll();
                CHECK(exhausted.status == Status::HypothesisLimitExceeded);
                CHECK_FALSE(exhausted.evidence);
                CHECK(fixture.stream->Start(fixture.provider) == Status::HypothesisLimitExceeded);
                CHECK(fixture.provider.opens == 1);
            }
            SECTION("PCM window exceeded") {
                fixture.StartWithAudio();
                const Capture::AudioCapturedBlock late{.start = {5, 16'000'000'000}, .firstFrame = 2, .sequence = 2, .frames = 2};
                CHECK(fixture.stream->Append(fixture.source, late, fixture.Samples) == Status::CaptureLimitExceeded);
                CHECK(fixture.provider.blocks == 1);
            }
        }

        TEST_CASE("Immersive speech never sends PCM beyond the capture deadline", "[agent][immersive][speech]") {
            SpeechFixture fixture;
            fixture.StartWithAudio();
            constexpr std::uint64_t duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(ImmersiveSpeechTranscription::MaximumInputDuration).count();
            constexpr std::uint64_t secondBlockLength = 41'667; // Two frames at 48 kHz, rounded up to nanoseconds.
            const auto lastStart = SpeechFixture::FirstBlock().start.nanoseconds + duration - secondBlockLength;
            Capture::AudioCapturedBlock last{.start = {5, lastStart}, .firstFrame = 2, .sequence = 2, .frames = 2};
            auto expected = Status::Accepted;
            SECTION("PCM ends exactly at the deadline") {}
            SECTION("PCM ends one nanosecond after the deadline") {
                ++last.start.nanoseconds;
                expected = Status::CaptureLimitExceeded;
            }
            CHECK(fixture.stream->Append(fixture.source, last, fixture.Samples) == expected);
            CHECK(fixture.provider.blocks == (expected == Status::Accepted ? 2 : 1));
        }
    }  // namespace
}  // namespace Horo::Editor
