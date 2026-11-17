#include "Horo/Runtime/Ui/UiCaptionSettings.h"
#include "Horo/Runtime/Ui/UiCaptions.h"
#include "UiAccessibilityTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Runtime::Ui {
    namespace {
        std::unique_ptr<CaptionCueQueue> Queue(const std::uint64_t session = 1,
                                               const CaptionProducerKind producer = CaptionProducerKind::Semantic) {
            auto text = LocalizedMessageRef::Create(UiMessageKey::Create("game.dialogue", "hello").Value(), {}, "Hello");
            return std::move(CaptionCueQueue::Create(session, {std::move(text).Value()}, producer)).Value();
        }

        CaptionCue Cue(const std::uint64_t sequence, const std::uint64_t start = 10) {
            return {{1, sequence}, CaptionCueKind::Dialogue, 0, 1, start, 20};
        }
    }  // namespace

    TEST_CASE("Caption callback transport does not allocate or reclaim catalog text", "[runtime_ui][captions][allocation]") {
        auto queue = Queue();
        const auto cue = Cue(1);
        CaptionCue consumed;
        const auto before = accessibilityAllocations.load();
        const auto admission = queue->TryEnqueue(cue);
        const auto dequeued = queue->TryDequeue(consumed);
        const auto after = accessibilityAllocations.load();
        REQUIRE(admission == CaptionAdmission::Accepted);
        REQUIRE(dequeued);
        REQUIRE(after == before);
    }

    TEST_CASE("Audio timing observations cannot create, replace or restart semantic captions", "[runtime_ui][captions][audio]") {
        CaptionTimeline timeline;
        REQUIRE(timeline.Reset(1).HasValue());
        const std::array<CaptionTimingObservation, 1> observed{{{{1, 1}, 12}}};
        REQUIRE(timeline.ObserveAudioTiming(observed).Value() == 0);
        auto semantic = Queue();
        REQUIRE(semantic->TryEnqueue(Cue(1)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*semantic) == 0);
        REQUIRE(timeline.ObserveAudioTiming(observed).Value() == 1);
        REQUIRE(timeline.ObserveAudioTiming(observed).Value() == 0);
        REQUIRE(timeline.Cues()[0].cue.startTick == 10);
        REQUIRE(timeline.Cues()[0].observedAudioStart == 12);
        auto audio = Queue(1, CaptionProducerKind::AudioOnly);
        REQUIRE(audio->TryEnqueue(Cue(2)) == CaptionAdmission::Invalid);
        auto sound = Cue(2);
        sound.kind = CaptionCueKind::Sound;
        REQUIRE(audio->TryEnqueue(sound) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*audio) == 0);
        const std::array<CaptionTimingObservation, 2> other{{{{1, 2}, 13}, {{2, 1}, 13}}};
        REQUIRE(timeline.ObserveAudioTiming(other).Value() == 0);
        timeline.BeginFrame(30);
        REQUIRE(timeline.Cues().empty());
        REQUIRE(timeline.ObserveAudioTiming(observed).Value() == 0);
        const std::array<CaptionTimingObservation, MaximumCaptionCues + 1> oversized{};
        REQUIRE(timeline.ObserveAudioTiming(oversized).HasError());
    }

    TEST_CASE("Caption appearance applies one complete live configuration revision", "[runtime_ui][captions][configuration]") {
        ConfigurationSchema schema;
        for (const auto &descriptor : MakeCaptionSettingsContribution().settings)
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationResolutionRequest inputs;
        const auto first = CaptionSettingsSnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 1).Value());
        REQUIRE(first.HasValue());
        REQUIRE_FALSE(first.Value().Appearance().enabled);
        inputs.user.emplace(SettingKey{"accessibility.captions.enabled"}, ConfigurationInputValue{true, {}});
        inputs.user.emplace(SettingKey{"accessibility.captions.font_size"}, ConfigurationInputValue{std::int64_t{1500}, {}});
        inputs.user.emplace(SettingKey{"accessibility.captions.background_opacity"}, ConfigurationInputValue{std::int64_t{500}, {}});
        inputs.user.emplace(SettingKey{"accessibility.captions.text_color"}, ConfigurationInputValue{std::int64_t{0xff0000ff}, {}});
        inputs.user.emplace(SettingKey{"accessibility.captions.position"}, ConfigurationInputValue{std::int64_t{1}, {}});
        inputs.user.emplace(SettingKey{"accessibility.captions.show_speaker"}, ConfigurationInputValue{false, {}});
        const auto second = CaptionSettingsSnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 2).Value());
        REQUIRE(second.HasValue());
        const auto &appearance = second.Value().Appearance();
        REQUIRE(second.Value().Revision() == 2);
        REQUIRE(appearance.enabled);
        REQUIRE(appearance.fontScale == 1.5F);
        REQUIRE(appearance.backgroundOpacity == 0.5F);
        REQUIRE(appearance.textColor == std::array<float, 4>{1, 0, 0, 1});
        REQUIRE(appearance.position == CaptionPosition::Top);
        REQUIRE_FALSE(appearance.showSpeaker);
        REQUIRE(first.Value().Revision() == 1);
        REQUIRE(first.Value().Appearance().fontScale == 1);
        inputs.user[SettingKey{"accessibility.captions.font_size"}].value = std::int64_t{499};
        REQUIRE(CaptionSettingsSnapshot::Capture(ConfigurationResolver::Resolve(schema, inputs, 3).Value()).HasError());
    }

    TEST_CASE("Caption safe areas preserve measured typography on reference aspect ratios", "[runtime_ui][captions][layout]") {
        CaptionAppearance appearance;
        appearance.enabled = true;
        const std::array<CaptionMeasuredCue, 2> text{{{{1, 1}, {320 * 64, 40 * 64}, {}}, {{1, 2}, {320 * 64, 40 * 64}, {}}}};
        for (const auto extent : {UiLogicalExtent{1920 * 64, 1080 * 64}, UiLogicalExtent{1024 * 64, 768 * 64},
                                  UiLogicalExtent{2560 * 64, 1080 * 64}, UiLogicalExtent{720 * 64, 1280 * 64}}) {
            const UiLogicalRect viewport{{0, 0}, extent};
            for (const auto anchor : {CaptionPosition::Top, CaptionPosition::Bottom}) {
                appearance.position = anchor;
                const auto result = ArrangeCaptions(viewport, appearance, text);
                REQUIRE(result.HasValue());
                REQUIRE(result.Value().count == 2);
                REQUIRE(result.Value().unplaced == 0);
                const auto &safe = result.Value().safeArea;
                for (std::size_t i = 0; i < result.Value().count; ++i) {
                    const auto &rect = result.Value().placements[i].bounds;
                    REQUIRE(rect.extent == text[i].extent);
                    REQUIRE(rect.origin.x >= safe.origin.x);
                    REQUIRE(rect.origin.y >= safe.origin.y);
                    REQUIRE(rect.origin.x + rect.extent.width <= safe.origin.x + safe.extent.width);
                    REQUIRE(rect.origin.y + rect.extent.height <= safe.origin.y + safe.extent.height);
                }
                REQUIRE(result.Value().placements[0].bounds.origin.y != result.Value().placements[1].bounds.origin.y);
            }
        }
        appearance.position = CaptionPosition::FollowSpeaker;
        const std::array<CaptionMeasuredCue, 1> speaker{{{{1, 1}, {320 * 64, 40 * 64}, UiLogicalPoint{-1000, 1000000}}}};
        const auto followed = ArrangeCaptions({{0, 0}, {640 * 64, 480 * 64}}, appearance, speaker);
        REQUIRE(followed.HasValue());
        REQUIRE(followed.Value().placements[0].bounds.origin.x == followed.Value().safeArea.origin.x);
        REQUIRE(followed.Value().placements[0].bounds.origin.y + 40 * 64 ==
                followed.Value().safeArea.origin.y + followed.Value().safeArea.extent.height);
        const auto narrow = ArrangeCaptions({{0, 0}, {100 * 64, 80 * 64}}, appearance, text);
        REQUIRE(narrow.HasValue());
        REQUIRE(narrow.Value().count == 0);
        REQUIRE(narrow.Value().unplaced == 2);
        appearance.fontScale = std::numeric_limits<float>::quiet_NaN();
        REQUIRE(ArrangeCaptions({{0, 0}, {100, 100}}, appearance, text).HasError());
    }

    TEST_CASE("Captions remain eligible independently of audio and keep localized catalog ownership", "[runtime_ui][captions]") {
        CaptionTimeline timeline;
        REQUIRE(timeline.Reset(1).HasValue());
        timeline.BeginFrame(10);
        {
            auto semantic = Queue();
            REQUIRE(semantic->TryEnqueue(Cue(1)) == CaptionAdmission::Accepted);
            REQUIRE(timeline.Drain(*semantic) == 0);
        }
        REQUIRE(timeline.Cues().size() == 1);
        const auto &catalog = *timeline.Cues()[0].catalog;
        REQUIRE(catalog[0].Key().Canonical() == "game.dialogue:hello");
        REQUIRE(catalog[0].FallbackText() == "Hello");
        auto audio = Queue();
        REQUIRE(audio->TryEnqueue(Cue(1, 20)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*audio) == 0);
        REQUIRE(timeline.Cues().size() == 1);
        REQUIRE(timeline.Cues()[0].cue.startTick == 10);
        timeline.BeginFrame(30);
        REQUIRE(timeline.Cues().empty());
        REQUIRE(audio->TryEnqueue(Cue(1, 30)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*audio) == 0);
        REQUIRE(timeline.Cues().empty());
    }

    TEST_CASE("Caption queues reject invalid values, stale sessions and overflow without replacing accepted data",
              "[runtime_ui][captions]") {
        auto queue = Queue();
        auto invalid = Cue(1);
        invalid.id.session = 2;
        REQUIRE(queue->TryEnqueue(invalid) == CaptionAdmission::StaleSession);
        invalid = Cue(1);
        invalid.durationTicks = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(queue->TryEnqueue(invalid) == CaptionAdmission::Invalid);
        invalid = Cue(1);
        invalid.text = 1;
        REQUIRE(queue->TryEnqueue(invalid) == CaptionAdmission::Invalid);
        for (std::size_t i = 1; i <= MaximumCaptionCues; ++i)
            REQUIRE(queue->TryEnqueue(Cue(i)) == CaptionAdmission::Accepted);
        REQUIRE(queue->TryEnqueue(Cue(33)) == CaptionAdmission::Full);
        REQUIRE(queue->Dropped() == 1);
        CaptionCue out;
        for (std::size_t i = 1; i <= MaximumCaptionCues; ++i) {
            REQUIRE(queue->TryDequeue(out));
            REQUIRE(out.id.sequence == i);
        }
        REQUIRE_FALSE(queue->TryDequeue(out));
        REQUIRE(queue->TryEnqueue(Cue(33)) == CaptionAdmission::Accepted);
    }

    TEST_CASE("Caption timeline orders pending cues and fences session, replay window and active capacity", "[runtime_ui][captions]") {
        auto queue = Queue();
        CaptionTimeline timeline;
        REQUIRE(timeline.Reset(1).HasValue());
        REQUIRE(queue->TryEnqueue(Cue(2, 20)) == CaptionAdmission::Accepted);
        REQUIRE(queue->TryEnqueue(Cue(1, 10)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*queue) == 0);
        REQUIRE(timeline.Cues()[0].cue.id.sequence == 1);
        REQUIRE(timeline.Cues()[1].cue.id.sequence == 2);
        for (std::uint64_t i = 3; i <= 32; ++i)
            REQUIRE(queue->TryEnqueue(Cue(i)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*queue) == 0);
        REQUIRE(queue->TryEnqueue(Cue(33)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*queue) == 1);
        REQUIRE(timeline.Reset(1).HasError());
        REQUIRE(timeline.Cues().size() == 32);
        REQUIRE(timeline.Reset(2).HasValue());
        REQUIRE(queue->TryEnqueue(Cue(34)) == CaptionAdmission::Accepted);
        REQUIRE(timeline.Drain(*queue) == 0);
        REQUIRE(timeline.Cues().empty());
        REQUIRE(timeline.Reset(1).HasError());
        CaptionTimeline replay;
        REQUIRE(replay.Reset(1).HasValue());
        REQUIRE(queue->TryEnqueue(Cue(100)) == CaptionAdmission::Accepted);
        REQUIRE(queue->TryEnqueue(Cue(1)) == CaptionAdmission::Accepted);
        REQUIRE(replay.Drain(*queue) == 0);
        REQUIRE(replay.Cues().size() == 1);
        replay.BeginFrame(11);
        replay.BeginFrame(10);
        REQUIRE(replay.Cues().empty());
        REQUIRE(replay.Reset(1).HasError());
    }

    TEST_CASE("Caption SPSC transport preserves complete payloads across callback and UI threads", "[runtime_ui][captions][threading]") {
        auto queue = Queue();
        constexpr std::uint64_t total = 10000;
        std::jthread producer([&](const std::stop_token stop) {
            for (std::uint64_t i = 1; i <= total; ++i) {
                while (queue->TryEnqueue(Cue(i, i)) == CaptionAdmission::Full) {
                    if (stop.stop_requested())
                        return;
                    std::this_thread::yield();
                }
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
        std::uint64_t received = 0;
        bool consistent = true;
        while (received < total) {
            CaptionCue cue;
            if (!queue->TryDequeue(cue)) {
                if (std::chrono::steady_clock::now() >= deadline)
                    break;
                std::this_thread::yield();
                continue;
            }
            ++received;
            consistent &= cue.id.sequence == received && cue.startTick == received && cue.speaker == 1;
        }
        producer.request_stop();
        producer.join();
        REQUIRE(received == total);
        REQUIRE(consistent);
    }
}  // namespace Horo::Runtime::Ui
