#include "Horo/Audio/Internal/AudioOutputPolicy.h"
#include "Horo/Audio/Internal/AudioRuntimeOwnerIssuer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo::Audio;
using namespace Horo::Audio::Backend;

namespace {
    TEST_CASE("Process composition never recycles issued Audio runtime owners", "[audio][policy][identity]") {
        AudioRuntimeOwnerIssuer owners;
        AudioRuntimeId previous;
        for (std::uint64_t generation = 1; generation <= 256; ++generation) {
            const auto issued = owners.Issue();
            REQUIRE(issued.HasValue());
            CHECK(issued.Value().IsValid());
            CHECK(issued.Value() != previous);
            CHECK(issued.Value().Value() == generation);
            previous = issued.Value();
        }
    }

    AudioDeviceEpoch Epoch(const std::uint64_t owner) {
        return {{AudioRuntimeId::Create(owner).Value(), 1, 1}, 1, 1};
    }

    TEST_CASE("Buffer profiles preserve hard limits and scale by rate", "[audio][policy]") {
        BufferPolicy policy;
        policy.profile = BufferProfile::LowLatency;
        const auto low = ResolveBufferPolicy(policy);
        REQUIRE(low.HasValue());
        REQUIRE(low.Value().preferredFrames == 240);
        policy.profile = BufferProfile::Stable;
        REQUIRE(ResolveBufferPolicy(policy).Value().preferredFrames == 960);
        policy.profile = BufferProfile::PowerSaving;
        REQUIRE(ResolveBufferPolicy(policy).Value().preferredFrames == 1920);
        policy.maximumFrames = 128;
        REQUIRE(ResolveBufferPolicy(policy).Value().preferredFrames == 128);
        policy.minimumFrames = 129;
        REQUIRE(ResolveBufferPolicy(policy).HasError());
        policy = {.profile = BufferProfile::LowLatency, .sampleRate = 44'100};
        REQUIRE(ResolveBufferPolicy(policy).Value().preferredFrames == 221);
        policy.profile = static_cast<BufferProfile>(255);
        REQUIRE(ResolveBufferPolicy(policy).HasError());
        policy = {};
        policy.sampleRate = 0;
        REQUIRE(ResolveBufferPolicy(policy).HasError());
    }

    TEST_CASE("Software latency remains an estimate and cannot wrap into a small latency", "[audio][policy]") {
        const auto estimate = EstimateSoftwareLatency({48, 24, 24}, 48'000, {7, 0});
        REQUIRE(estimate.HasValue());
        REQUIRE(estimate.Value().nanoseconds == 2'000'000);
        REQUIRE(estimate.Value().quality == AudioObservationQuality::Estimated);
        REQUIRE(estimate.Value().source == AudioObservationSource::Adapter);
        REQUIRE(ValidateAudioDurationObservation(estimate.Value()));
        REQUIRE(EstimateSoftwareLatency({1, 0, 0}, 48'000, {7, 0}).Value().nanoseconds == 20'834);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(EstimateSoftwareLatency({maximum, 1, 0}, 48'000, {7, 0}).HasError());
        REQUIRE(EstimateSoftwareLatency({maximum, 0, 0}, 48'000, {7, 0}).HasError());
        REQUIRE(EstimateSoftwareLatency({}, 0, {7, 0}).HasError());
        REQUIRE(EstimateSoftwareLatency({}, 48'000, {}).HasError());
    }

    TEST_CASE("Game and preview require independent runtimes and never silently mirror", "[audio][policy]") {
        std::array outputs{OutputBinding{OutputUse::Game, Epoch(1)}, OutputBinding{OutputUse::EditorPreview, Epoch(2)}};
        REQUIRE(AdmitOutputComposition(outputs, OutputComposition::Independent).HasValue());
        REQUIRE(AdmitOutputComposition(outputs, OutputComposition::Mirrored).HasError());
        REQUIRE(AdmitOutputComposition(outputs, OutputComposition::Aggregate).HasError());
        REQUIRE(AdmitOutputComposition(outputs, static_cast<OutputComposition>(255)).HasError());
        outputs[1].epoch = Epoch(1);
        REQUIRE(AdmitOutputComposition(outputs, OutputComposition::Independent).HasError());
        outputs[1] = outputs[0];
        outputs[1].epoch = Epoch(2);
        REQUIRE(AdmitOutputComposition(outputs, OutputComposition::Independent).HasError());
        REQUIRE(AdmitOutputComposition({}, OutputComposition::Independent).HasError());
    }

    TEST_CASE("Reconfiguration publishes new actual facts and discards stale epoch timing without rewriting intent", "[audio][policy]") {
        Open request{.plannedEpoch = Epoch(1),
                     .format = {.device = Epoch(1).device,
                                .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                .period = {64, 256, 512}}};
        const AudioDeviceSnapshot devices{.owner = Epoch(1).device.owner,
                                          .backend = AudioBackendKind::SDL3Audio,
                                          .revision = 1,
                                          .devices = {{Epoch(1).device, "Fixture", AudioDeviceClass::Virtual}}};
        Opened actual{.format = {.device = Epoch(1).device,
                                 .discoveryRevision = 1,
                                 .formatRevision = 1,
                                 .effective = request.format.preferred,
                                 .nativeSignal = request.format.preferred,
                                 .nativePcm = {},
                                 .nativeChannelForHoro = {0, 1},
                                 .callbackFrames = 128,
                                 .nativePeriodFrames = 128,
                                 .deviations = {.callbackPeriod = AudioFormatDeviationReason::DeviceConstraint}},
                      .timing = {.epoch = Epoch(1), .capturedAt = {7, 100}}};
        auto prepared = PrepareOutputDiagnostics(request, devices, actual, {48, 0, 48});
        REQUIRE(prepared.HasValue());
        auto &snapshot = prepared.Value();
        REQUIRE(snapshot.requested.format.period.preferredFrames == 256);
        REQUIRE(snapshot.actual.format.callbackFrames == 128);
        REQUIRE(snapshot.softwareLatency.quality == AudioObservationQuality::Estimated);
        auto timing = actual.timing;
        timing.capturedAt.nanoseconds = 200;
        timing.hardwareLatency = {.quality = AudioObservationQuality::Reported,
                                  .source = AudioObservationSource::BackendApi,
                                  .nanoseconds = 5'000'000,
                                  .observedAt = timing.capturedAt};
        REQUIRE(UpdateOutputTiming(snapshot, timing));
        REQUIRE(snapshot.actual.timing.hardwareLatency.quality == AudioObservationQuality::Reported);
        ++request.plannedEpoch.callbackEpoch;
        ++request.plannedEpoch.formatRevision;
        actual.timing = {.epoch = request.plannedEpoch, .capturedAt = {7, 300}};
        actual.format.formatRevision = request.plannedEpoch.formatRevision;
        auto replacement = PrepareOutputDiagnostics(request, devices, actual, {});
        REQUIRE(replacement.HasValue());
        REQUIRE(replacement.Value().actual.timing.hardwareLatency.quality == AudioObservationQuality::Unknown);
        REQUIRE_FALSE(UpdateOutputTiming(replacement.Value(), timing));
        REQUIRE(replacement.Value().actual.timing.capturedAt.nanoseconds == 300);
        timing = actual.timing;
        timing.capturedAt.nanoseconds = 299;
        REQUIRE_FALSE(UpdateOutputTiming(replacement.Value(), timing));
    }
}  // namespace
