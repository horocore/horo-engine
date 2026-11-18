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
                    REQUIRE(backend.AcknowledgeCompletion(operation.Value()).HasValue());
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
            const AudioDeviceFormatRequest request{.device = selected.device,
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

        /** @brief Proves native detachment before releasing the port and drains the previous epoch's facts. */
        void CloseOutput(NativeAudioBackend &backend, const AudioDeviceEpoch &epoch) {
            CHECK(Complete<Quiesced>(backend, Quiesce{epoch}).epoch == epoch);
            CHECK(Complete<Stopped>(backend, Stop{epoch}).epoch == epoch);
            Complete<Closed>(backend, Close{});
            std::array<Event, 16> events;
            while (backend.DrainEvents(events)) {
            }
        }
    }  // namespace

    TEST_CASE("PipeWire retires callbacks after actual service loss and reopens with a new format", "[audio][pipewire][native]") {
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

        RenderTrace oldTrace;
        const auto before = OpenAvailable(*backend, 48000, 1);
        StartRendering(*backend, before, oldTrace);
        std::ofstream(control / "started").put('\n');
        REQUIRE(WaitFor(*backend, [&before](const Event &event) {
            const auto *lost = std::get_if<DeviceLost>(&event.fact);
            return lost && lost->device == before.device;
        }));
        CloseOutput(*backend, before);
        const auto stoppedCalls = oldTrace.calls.load(std::memory_order_relaxed);
        std::ofstream(control / "detached").put('\n');

        const auto end = Clock::now() + 5s;
        while (Clock::now() < end && !std::filesystem::exists(control / "restarted"))
            std::this_thread::sleep_for(2ms);
        REQUIRE(std::filesystem::exists(control / "restarted"));
        RenderTrace newTrace;
        const auto after = OpenAvailable(*backend, 44100, 2);
        CHECK(after.device != before.device);
        CHECK(backend->Begin(Start{before, {&oldTrace, Render}}, Deadline(Clock::now() + 1s)).HasError());
        StartRendering(*backend, after, newTrace);
        CHECK(oldTrace.calls.load(std::memory_order_relaxed) == stoppedCalls);
        CloseOutput(*backend, after);
        CHECK(oldTrace.calls.load(std::memory_order_relaxed) == stoppedCalls);
    }
}  // namespace Horo::Audio::Backend
