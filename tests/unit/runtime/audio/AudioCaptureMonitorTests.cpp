#include "Horo/Audio/Internal/AudioCaptureMonitor.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Audio::Backend {
    namespace {
        /** @brief Prepared exact capture and output identities for a confirmed private local monitor. */
        struct MonitorFixture final {
            const AudioRuntimeId owner = AudioRuntimeId::Create(9).Value();
            const AudioCaptureBufferDescriptor capture{.device = {owner, 4, 1},
                                                       .generation = 3,
                                                       .clockDomain = 5,
                                                       .format = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                       .slots = 2,
                                                       .maximumFrames = 2,
                                                       .budgetBytes = 4096};
            const AudioDeviceEpoch output{{owner, 6, 2}, 1, 4};
            const AudioCaptureMonitorRoute route{.input = capture.device,
                                                 .captureGeneration = capture.generation,
                                                 .output = output,
                                                 .bus = {owner, 7, 2},
                                                 .clockDomain = capture.clockDomain,
                                                 .maximumAgeNanoseconds = 200'000'000,
                                                 .gain = 0.5F,
                                                 .isolation = AudioMonitorIsolation::PrivateListening,
                                                 .echoReference = AudioEchoReferenceTap::ProgramBeforeMonitor};

            AudioCaptureBuffer Buffer() const {
                auto buffer = AudioCaptureBuffer::Create(capture);
                REQUIRE(buffer.HasValue());
                return std::move(buffer).Value();
            }
        };

        TEST_CASE("Local capture monitoring rejects unsafe, unscoped and replaced routes", "[audio][capture][monitor]") {
            const MonitorFixture fixture;
            REQUIRE(AdmitAudioCaptureMonitor(fixture.route, fixture.capture, fixture.output).HasValue());
            auto route = fixture.route;
            SECTION("the host has not verified private listening") {
                route.isolation = AudioMonitorIsolation::Unverified;
            }
            SECTION("an input generation changed") {
                ++route.captureGeneration;
            }
            SECTION("the destination callback epoch is stale") {
                ++route.output.callbackEpoch;
            }
            SECTION("the bus is missing") {
                route.bus = {};
            }
            SECTION("gain would amplify the microphone") {
                route.gain = 2.0F;
            }
            SECTION("latency is unbounded") {
                route.maximumAgeNanoseconds = std::numeric_limits<std::uint64_t>::max();
            }
            CHECK(AdmitAudioCaptureMonitor(route, fixture.capture, fixture.output).HasError());
        }

        TEST_CASE("Local monitor applies gain and clears expired samples before delivery", "[audio][capture][monitor]") {
            const MonitorFixture fixture;
            auto buffer = fixture.Buffer();
            const std::array input{0.5F, -3.0F};
            REQUIRE(buffer.TryWrite({5, 1'000}, 0, 1, input) == AudioCaptureWrite::Accepted);
            REQUIRE(buffer.TryWrite({5, 22'000}, 1, 1, input) == AudioCaptureWrite::Accepted);
            AudioCapturedBlock block;
            std::array<AudioSample, 3> pcm{0.0F, 0.0F, 99.0F};
            auto stale = fixture.route;
            ++stale.captureGeneration;
            CHECK(ReadAudioMonitorBlock(buffer, stale, {5, 21'000}, block, pcm) == AudioMonitorRead::Cancelled);
            auto wrongBus = fixture.route;
            wrongBus.bus.owner = AudioRuntimeId::Create(10).Value();
            CHECK(ReadAudioMonitorBlock(buffer, wrongBus, {5, 21'000}, block, pcm) == AudioMonitorRead::Cancelled);
            CHECK((pcm == std::array{0.0F, 0.0F, 99.0F}));
            REQUIRE(ReadAudioMonitorBlock(buffer, fixture.route, {5, 21'000}, block, pcm) == AudioMonitorRead::Copied);
            CHECK(block.sequence == 1);
            CHECK((pcm == std::array{0.25F, -1.0F, 99.0F}));
            CHECK(ReadAudioMonitorBlock(buffer, fixture.route, {5, 200'022'001}, block, pcm) == AudioMonitorRead::Expired);
            CHECK(block.sequence == 1);
            CHECK((pcm == std::array{0.0F, 0.0F, 99.0F}));
        }

        TEST_CASE("Echo reference accepts only a current opted-in pre-monitor program tap", "[audio][capture][monitor]") {
            const MonitorFixture fixture;
            const std::array input{0.5F, -0.25F};
            AudioEchoReferenceBlock reference{fixture.output,
                                              AudioEchoReferenceTap::ProgramBeforeMonitor,
                                              {5, 1},
                                              fixture.capture.format,
                                              1,
                                              input};
            auto route = fixture.route;
            REQUIRE(AcceptAudioEchoReference(route, reference, fixture.output, {5, 2}));
            SECTION("reference was not opted in") {
                route.echoReference = AudioEchoReferenceTap::Disabled;
            }
            SECTION("the capture identity is missing") {
                route.input = {};
            }
            SECTION("the capture generation is missing") {
                route.captureGeneration = 0;
            }
            SECTION("the bus belongs to another output runtime") {
                route.bus.owner = AudioRuntimeId::Create(10).Value();
            }
            SECTION("the output changed") {
                ++reference.output.formatRevision;
            }
            SECTION("a different clock supplied the reference") {
                ++reference.start.clockDomain;
            }
            SECTION("the frame payload is incomplete") {
                ++reference.frames;
            }
            CHECK_FALSE(AcceptAudioEchoReference(route, reference, fixture.output, {5, 2}));
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
