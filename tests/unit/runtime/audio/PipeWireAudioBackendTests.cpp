#include "Horo/Audio/Internal/PipeWireAudioBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <thread>

namespace Horo::Audio::Backend {
    namespace {
        using namespace std::chrono_literals;
        using Clock = std::chrono::steady_clock;
        constexpr std::uint64_t ClockDomain = 600;

        /** @brief Captures a deadline in the test's declared host clock domain. */
        AudioMonotonicTimestamp Deadline(Clock::time_point end) {
            const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
            return {ClockDomain, static_cast<std::uint64_t>(nanos)};
        }

        /** @brief Advances one actual accepted backend operation without fabricating callback progress. */
        template <class Expected> Expected Complete(NativeAudioBackend &backend, const Request &request) {
            const auto end = Clock::now() + 4s;
            auto operation = backend.Begin(request, Deadline(end));
            REQUIRE(operation.HasValue());
            while (Clock::now() < end) {
                REQUIRE(backend.AdvanceControl().HasValue());
                auto result = backend.Poll(operation.Value());
                REQUIRE(result.HasValue());
                if (result.Value()) {
                    auto completed = std::move(*result.Value());
                    REQUIRE(completed.operation == operation.Value());
                    const auto retained = backend.Poll(operation.Value());
                    REQUIRE(retained.HasValue());
                    REQUIRE(retained.Value().has_value());
                    CHECK(retained.Value()->operation == completed.operation);
                    CHECK(retained.Value()->outcome.index() == completed.outcome.index());
                    CHECK(backend.Begin(request, Deadline(end)).HasError());
                    REQUIRE(backend.AcknowledgeCompletion(operation.Value()).HasValue());
                    CHECK(backend.Poll(operation.Value()).HasError());
                    if (const auto *failure = std::get_if<Failed>(&completed.outcome))
                        WARN("Audio request " << request.index() << ": " << failure->error.message);
                    REQUIRE(std::holds_alternative<Expected>(completed.outcome));
                    return std::get<Expected>(std::move(completed.outcome));
                }
                std::this_thread::sleep_for(2ms);
            }
            FAIL("Native audio operation did not retire within its deadline");
            std::terminate();
        }

        /** @brief Waits for a typed fact on the real native callback and control mailbox. */
        template <class Predicate> bool WaitFor(NativeAudioBackend &backend, Predicate matches) {
            const auto end = Clock::now() + 5s;
            std::array<Event, 16> events;
            while (Clock::now() < end) {
                const auto count = backend.DrainEvents(events);
                if (std::ranges::any_of(std::span{events}.first(count), matches))
                    return true;
                std::this_thread::sleep_for(2ms);
            }
            return false;
        }

        struct RenderTrace final {
            std::atomic<std::uint64_t> rendering{};
            std::atomic<std::uint64_t> calls{};
        };

        /** @brief Generates a constant bounded signal on the actual PipeWire callback lane. */
        RenderResult Render(void *context, const RenderInvocation &invocation) noexcept {
            auto &trace = *static_cast<RenderTrace *>(context);
            trace.calls.fetch_add(1, std::memory_order_relaxed);
            if (invocation.phase == RenderPhase::Priming)
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            if (invocation.phase == RenderPhase::Quiescing)
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            for (auto *plane : invocation.output.planes)
                std::fill_n(plane, invocation.output.validFrames, 0.125F);
            trace.rendering.fetch_add(1, std::memory_order_release);
            return {RenderDisposition::Rendered, AudioCallbackFaultCode::None};
        }

        /** @brief Reads a fresh device catalog and exposes the negotiated native stream format before Start. */
        AudioDeviceEpoch OpenAvailable(NativeAudioBackend &backend, std::uint32_t rate, std::uint64_t generation) {
            const auto snapshot = Complete<AudioDeviceSnapshot>(backend, Enumerate{});
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            const auto selected = ResolveAudioDevice(snapshot, AudioDefaultDeviceRole::Multimedia);
            REQUIRE(selected.status == AudioDeviceResolutionStatus::Resolved);
            const AudioDeviceEpoch epoch{selected.device, generation, generation};
            const AudioDeviceFormatRequest request{.device = AudioDefaultDeviceRole::Multimedia,
                                                   .preferred = {rate, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                   .period = {64, 256, 2048}};
            const auto opened = Complete<Opened>(backend, Open{epoch, request});
            CHECK(opened.format.nativeSignal.sampleRate == rate);
            CHECK(opened.format.effective == request.preferred);
            CHECK(opened.format.formatRevision == generation);
            CHECK(opened.format.nativeChannelForHoro.size() == 2);
            CHECK_FALSE(opened.format.nativePeriodFrames.has_value());
            CHECK(ValidateAudioDeviceNegotiation(request, snapshot, opened.format).status == AudioDeviceNegotiationStatus::Accepted);
            return epoch;
        }

        /** @brief Requires native callback readiness and actual rendered blocks for this exact epoch. */
        void StartRendering(NativeAudioBackend &backend, const AudioDeviceEpoch &epoch, RenderTrace &trace) {
            CHECK(Complete<Started>(backend, Start{epoch, {&trace, Render}}).epoch == epoch);
            CHECK_FALSE(backend.ReadPlaybackLatency(epoch).has_value());
            REQUIRE(WaitFor(backend, [&epoch](const Event &event) {
                const auto *callback = std::get_if<AudioCallbackEvent>(&event.fact);
                return callback && callback->epoch == epoch && std::holds_alternative<AudioCallbackReady>(callback->fact);
            }));
            REQUIRE(backend.CommitRendering(epoch).HasValue());
            const auto end = Clock::now() + 2s;
            while (Clock::now() < end && trace.rendering.load(std::memory_order_acquire) == 0)
                std::this_thread::sleep_for(2ms);
            REQUIRE(trace.rendering.load(std::memory_order_acquire) != 0);
        }

        /** @brief Waits for a genuine native snapshot captured after this output epoch began rendering. */
        std::optional<PlaybackLatencyEstimate> WaitForLatency(NativeAudioBackend &backend, const AudioDeviceEpoch &epoch) {
            const auto end = Clock::now() + 2s;
            while (Clock::now() < end) {
                if (auto timing = backend.ReadPlaybackLatency(epoch))
                    return timing;
                std::this_thread::sleep_for(2ms);
            }
            return std::nullopt;
        }

        /** @brief Proves native detachment before releasing the port and drains the previous epoch's facts. */
        void CloseOutput(NativeAudioBackend &backend, const AudioDeviceEpoch &epoch) {
            CHECK(Complete<Quiesced>(backend, Quiesce{epoch}).epoch == epoch);
            CHECK(Complete<Stopped>(backend, Stop{epoch}).epoch == epoch);
            Complete<Closed>(backend, Close{});
            std::array<Event, 16> events;
            while (backend.DrainEvents(events)) {
            }
        }

        /** @brief Verifies the actual PipeWire recorder default and removal before opening playback. */
        void VerifyCaptureRefresh(AudioBackend &backend, const std::filesystem::path &control) {
            const auto first = backend.DiscoverCapture();
            REQUIRE(first.HasValue());
            const auto &before = first.Value().snapshot;
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(before));
            REQUIRE(before.devices.size() == 1);
            CHECK(before.devices.front().displayName == "horo-test-mic-a");
            CHECK(before.permission == AudioCapturePermission::Unknown);
            CHECK_FALSE(before.defaults.communications);
            const auto selected = ResolveAudioCaptureDevice(before, AudioDefaultDeviceRole::Multimedia);
            REQUIRE(selected.status == AudioDeviceResolutionStatus::Resolved);
            CHECK(selected.device == before.devices.front().id);
            REQUIRE(first.Value().changes.size() == 1);
            CHECK(first.Value().changes.front().kind == AudioCaptureDeviceChangeKind::Added);
            const auto unchanged = backend.DiscoverCapture();
            REQUIRE(unchanged.HasValue());
            CHECK(unchanged.Value().snapshot.revision == before.revision);
            CHECK(unchanged.Value().changes.empty());

            std::ofstream(control / "capture-ready").put('\n');
            const auto deadline = Clock::now() + 5s;
            while (Clock::now() < deadline && !std::filesystem::exists(control / "capture-changed"))
                std::this_thread::sleep_for(2ms);
            REQUIRE(std::filesystem::exists(control / "capture-changed"));

            const auto next = backend.DiscoverCapture();
            REQUIRE(next.HasValue());
            const auto &after = next.Value().snapshot;
            REQUIRE(after.devices.size() == 1);
            CHECK(after.devices.front().displayName == "horo-test-mic-b");
            CHECK(after.revision > before.revision);
            CHECK(ResolveAudioCaptureDevice(after, selected.device).status == AudioDeviceResolutionStatus::Unavailable);
            CHECK(ResolveAudioCaptureDevice(after, AudioDefaultDeviceRole::Multimedia).device == after.devices.front().id);
            REQUIRE(next.Value().changes.size() == 2);
            CHECK(next.Value().changes[0].kind == AudioCaptureDeviceChangeKind::Removed);
            CHECK(next.Value().changes[0].device == selected.device);
            CHECK(next.Value().changes[1].kind == AudioCaptureDeviceChangeKind::Added);
            CHECK(next.Value().changes[1].device == after.devices.front().id);
        }
    }  // namespace

    TEST_CASE("PipeWire reports a live default switch and fences explicit new output epochs", "[audio][pipewire][native]") {
        const auto *controlDirectory = std::getenv("HORO_PIPEWIRE_TEST_CONTROL");
        REQUIRE(controlDirectory != nullptr);
        const std::filesystem::path control{controlDirectory};
        const auto owner = AudioRuntimeId::Create(ClockDomain);
        REQUIRE(owner.HasValue());
        auto library = InitializePipeWireAudioLibrary();
        REQUIRE(library.HasValue());
        auto created = CreatePipeWireAudioBackend({owner.Value(), ClockDomain}, library.Value());
        REQUIRE(created.HasValue());
        auto backend = std::move(created).Value();
        const auto probe = Complete<AudioBackendProbe>(*backend, Probe{});
        CHECK(probe.backend == AudioBackendKind::PipeWire);
        CHECK(probe.availability == AudioBackendAvailability::Available);
        VerifyCaptureRefresh(*backend, control);

        RenderTrace oldTrace;
        const auto before = OpenAvailable(*backend, 48000, 1);
        CHECK_FALSE(backend->ReadPlaybackLatency(before).has_value());
        StartRendering(*backend, before, oldTrace);
        CHECK(backend->DiscoverCapture().HasError());
        const auto oldEstimate = WaitForLatency(*backend, before);
        CHECK(oldEstimate.has_value());
        if (oldEstimate) {
            CHECK(oldEstimate->epoch == before);
            CHECK(oldEstimate->latency.quality == AudioObservationQuality::Estimated);
            CHECK(oldEstimate->latency.source == AudioObservationSource::BackendApi);
            CHECK(ValidateAudioDurationObservation(oldEstimate->latency));
        }

        std::ofstream(control / "switch-ready").put('\n');
        const auto switchDeadline = Clock::now() + 5s;
        while (Clock::now() < switchDeadline && !std::filesystem::exists(control / "default-changed"))
            std::this_thread::sleep_for(2ms);
        REQUIRE(std::filesystem::exists(control / "default-changed"));
        const auto refreshed = Complete<AudioDeviceSnapshot>(*backend, Enumerate{});
        const auto selectedDefault = ResolveAudioDevice(refreshed, AudioDefaultDeviceRole::Multimedia);
        REQUIRE(selectedDefault.status == AudioDeviceResolutionStatus::Resolved);
        CHECK(selectedDefault.device != before.device);
        CHECK(ResolveAudioDevice(refreshed, before.device).device == before.device);
        CHECK(WaitFor(*backend, [&](const Event &event) {
            const auto *change = std::get_if<DefaultChanged>(&event.fact);
            return change && event.owner == before.device.owner && change->role == AudioDefaultDeviceRole::Multimedia &&
                   change->discoveryRevision == refreshed.revision && change->device == selectedDefault.device;
        }));
        const auto renderedBeforeSwitch = oldTrace.rendering.load(std::memory_order_acquire);
        const auto renderDeadline = Clock::now() + 2s;
        while (Clock::now() < renderDeadline && oldTrace.rendering.load(std::memory_order_acquire) == renderedBeforeSwitch)
            std::this_thread::sleep_for(2ms);
        CHECK(oldTrace.rendering.load(std::memory_order_acquire) > renderedBeforeSwitch);
        CloseOutput(*backend, before);
        const auto stoppedCalls = oldTrace.calls.load(std::memory_order_relaxed);

        RenderTrace switchedTrace;
        const auto switched = OpenAvailable(*backend, 44100, 2);
        CHECK(switched.device == selectedDefault.device);
        CHECK_FALSE(backend->ReadPlaybackLatency(before).has_value());
        CHECK(backend->Begin(Start{before, {&oldTrace, Render}}, Deadline(Clock::now() + 1s)).HasError());
        StartRendering(*backend, switched, switchedTrace);
        CHECK(oldTrace.calls.load(std::memory_order_relaxed) == stoppedCalls);
        std::ofstream(control / "started").put('\n');
        REQUIRE(WaitFor(*backend, [&switched](const Event &event) {
            const auto *lost = std::get_if<DeviceLost>(&event.fact);
            return lost && lost->epoch == switched;
        }));
        CHECK_FALSE(backend->ReadPlaybackLatency(switched).has_value());
        CloseOutput(*backend, switched);
        const auto switchedCalls = switchedTrace.calls.load(std::memory_order_relaxed);
        std::ofstream(control / "detached").put('\n');

        const auto end = Clock::now() + 5s;
        while (Clock::now() < end && !std::filesystem::exists(control / "restarted"))
            std::this_thread::sleep_for(2ms);
        REQUIRE(std::filesystem::exists(control / "restarted"));
        RenderTrace newTrace;
        const auto after = OpenAvailable(*backend, 48000, 3);
        CHECK(after.device != switched.device);
        CHECK_FALSE(backend->ReadPlaybackLatency(after).has_value());
        CHECK(backend->Begin(Start{switched, {&switchedTrace, Render}}, Deadline(Clock::now() + 1s)).HasError());
        StartRendering(*backend, after, newTrace);
        const auto estimate = WaitForLatency(*backend, after);
        CHECK(estimate.has_value());
        if (estimate) {
            CHECK(estimate->epoch == after);
            CHECK(estimate->latency.quality == AudioObservationQuality::Estimated);
            CHECK(estimate->latency.source == AudioObservationSource::BackendApi);
            CHECK(ValidateAudioDurationObservation(estimate->latency));
            if (oldEstimate && oldEstimate->latency.observedAt && estimate->latency.observedAt)
                CHECK(estimate->latency.observedAt->nanoseconds > oldEstimate->latency.observedAt->nanoseconds);
        }
        CHECK_FALSE(backend->ReadPlaybackLatency(before).has_value());
        CHECK(oldTrace.calls.load(std::memory_order_relaxed) == stoppedCalls);
        CHECK(switchedTrace.calls.load(std::memory_order_relaxed) == switchedCalls);
        CloseOutput(*backend, after);
        CHECK_FALSE(backend->ReadPlaybackLatency(after).has_value());
        CHECK(oldTrace.calls.load(std::memory_order_relaxed) == stoppedCalls);
        CHECK(switchedTrace.calls.load(std::memory_order_relaxed) == switchedCalls);
    }
}  // namespace Horo::Audio::Backend
