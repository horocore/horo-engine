#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Audio::Backend {
    namespace {
        AudioRuntimeId Owner() {
            return AudioRuntimeId::Create(597).Value();
        }

        Sdl3AudioBackendConfig Config() {
            return {.owner = Owner(), .clockDomain = 31};
        }

        OperationId Begin(Sdl3AudioBackend &backend, const Request &request) {
            const auto result = backend.Begin(request, {Config().clockDomain, 1});
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Completion Complete(Sdl3AudioBackend &backend, const Request &request) {
            const auto operation = Begin(backend, request);
            REQUIRE(backend.AdvanceControl().HasValue());
            const auto result = backend.Poll(operation);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            const auto completion = *result.Value();
            REQUIRE(backend.AcknowledgeCompletion(operation).HasValue());
            return completion;
        }

        struct Trace final {
            std::atomic<std::size_t> calls{};
            std::atomic<bool> invalidResponse{};
        };

        RenderResult Render(void *context, const RenderInvocation &invocation) noexcept {
            auto &trace = *static_cast<Trace *>(context);
            trace.calls.fetch_add(1, std::memory_order_relaxed);
            for (auto *plane : invocation.output.planes)
                std::fill_n(plane, invocation.output.validFrames, 0.125F);
            if (trace.invalidResponse.load(std::memory_order_relaxed))
                return {RenderDisposition::Fault, AudioCallbackFaultCode::None};
            if (invocation.phase == RenderPhase::Priming)
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            if (invocation.phase == RenderPhase::Quiescing)
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            return {RenderDisposition::Rendered, AudioCallbackFaultCode::None};
        }

        enum class ExpectedCallbackEvent : std::uint8_t {
            Ready,
            Quiesced,
            Fault
        };

        bool Matches(const AudioCallbackFact &fact, const ExpectedCallbackEvent expected) {
            if (expected == ExpectedCallbackEvent::Ready)
                return std::holds_alternative<AudioCallbackReady>(fact);
            if (expected == ExpectedCallbackEvent::Quiesced)
                return std::holds_alternative<AudioCallbackQuiesced>(fact);
            const auto *fault = std::get_if<AudioCallbackFault>(&fact);
            return fault && fault->code == AudioCallbackFaultCode::BackendFailure;
        }

        bool WaitForEvent(Sdl3AudioBackend &backend, const ExpectedCallbackEvent expected) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            std::array<Event, 16> events;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto count = backend.DrainEvents(events);
                for (std::size_t index = 0; index < count; ++index) {
                    const auto *callback = std::get_if<AudioCallbackEvent>(&events[index].fact);
                    if (!callback)
                        continue;
                    if (Matches(callback->fact, expected))
                        return true;
                }
                std::this_thread::yield();
            }
            return false;
        }

        AudioDeviceEpoch OpenDefault(Sdl3AudioBackend &backend) {
            const auto probe = Complete(backend, Probe{});
            const auto &capabilities = std::get<AudioBackendProbe>(probe.outcome);
            REQUIRE(capabilities.availability == AudioBackendAvailability::Available);
            REQUIRE(capabilities.features[static_cast<std::size_t>(AudioBackendCapability::PhysicalEnumeration)] ==
                    AudioCapabilitySupport::Available);
            REQUIRE(capabilities.features[static_cast<std::size_t>(AudioBackendCapability::NativeDiagnostics)] ==
                    AudioCapabilitySupport::Available);
            REQUIRE(capabilities.features[static_cast<std::size_t>(AudioBackendCapability::NativeHotplug)] ==
                    AudioCapabilitySupport::Unsupported);
            const auto catalog = std::get<AudioDeviceSnapshot>(Complete(backend, Enumerate{}).outcome);
            REQUIRE(ValidateAudioDeviceSnapshot(catalog));
            REQUIRE_FALSE(catalog.devices.empty());

            const AudioDeviceEpoch epoch{catalog.devices.front().id, 1, 1};
            const AudioDeviceFormatRequest format{.device = AudioDefaultDeviceRole::Multimedia,
                                                  .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                  .period = {64, 256, 2'048},
                                                  .nativeRatePolicy = AudioDeviceRatePolicy::AllowPreparedResampler};
            const auto opened = std::get<Opened>(Complete(backend, Open{epoch, format, AccessMode::Shared}).outcome);
            REQUIRE(opened.format.device == epoch.device);
            REQUIRE(ValidateAudioDeviceNegotiation(format, catalog, opened.format).status == AudioDeviceNegotiationStatus::Accepted);
            return epoch;
        }

        void StartRendering(Sdl3AudioBackend &backend, const AudioDeviceEpoch &epoch, Trace &trace) {
            REQUIRE(std::holds_alternative<Started>(Complete(backend, Start{epoch, {&trace, Render}}).outcome));
            REQUIRE(WaitForEvent(backend, ExpectedCallbackEvent::Ready));
        }

        TEST_CASE("SDL3 audio drives the Horo render port and detaches before close", "[unit][audio][sdl3]") {
            auto created = CreateSdl3AudioBackend(Config());
            REQUIRE(created.HasValue());
            auto backend = std::move(created).Value();
            REQUIRE(backend->Kind() == AudioBackendKind::SDL3Audio);
            const auto epoch = OpenDefault(*backend);
            Trace trace;
            StartRendering(*backend, epoch, trace);
            REQUIRE(backend->CommitRendering(epoch).HasValue());
            REQUIRE(trace.calls.load(std::memory_order_relaxed) > 0);
            trace.invalidResponse.store(true, std::memory_order_relaxed);
            REQUIRE(WaitForEvent(*backend, ExpectedCallbackEvent::Fault));
            trace.invalidResponse.store(false, std::memory_order_relaxed);

            const auto quiesce = Begin(*backend, Quiesce{epoch});
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(WaitForEvent(*backend, ExpectedCallbackEvent::Quiesced));
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(std::holds_alternative<Quiesced>(backend->Poll(quiesce).Value()->outcome));
            REQUIRE(backend->AcknowledgeCompletion(quiesce).HasValue());
            REQUIRE(std::holds_alternative<Stopped>(Complete(*backend, Stop{epoch}).outcome));
            REQUIRE(std::holds_alternative<Closed>(Complete(*backend, Close{}).outcome));
        }

        TEST_CASE("SDL3 audio rejects malformed construction and lifecycle requests", "[unit][audio][sdl3]") {
            REQUIRE(CreateSdl3AudioBackend({}).HasError());
            auto backend = std::move(CreateSdl3AudioBackend(Config())).Value();
            const OperationId stale{Owner(), 99};
            REQUIRE(backend->AdvanceControl().HasError());
            REQUIRE(backend->Poll(stale).HasError());
            REQUIRE(backend->AcknowledgeCompletion(stale).HasError());
            REQUIRE(backend->Cancel(stale).HasError());
            std::array<Event, 1> events;
            REQUIRE(backend->DrainEvents(events) == 0);

            const auto probe = Begin(*backend, Probe{});
            REQUIRE(backend->Begin(Probe{}, {Config().clockDomain, 1}).HasError());
            REQUIRE(backend->Cancel(probe).Value() == CancelDisposition::Requested);
            REQUIRE(backend->Cancel(probe).Value() == CancelDisposition::AlreadyTerminal);
            REQUIRE(std::holds_alternative<Cancelled>(backend->Poll(probe).Value()->outcome));
            REQUIRE(backend->AcknowledgeCompletion(probe).HasValue());
            REQUIRE(backend->CommitRendering({}).HasError());
            REQUIRE(backend->Begin(Start{}, {Config().clockDomain, 1}).HasError());
            REQUIRE(backend->Begin(Probe{}, {Config().clockDomain + 1, 1}).HasError());
        }

        TEST_CASE("SDL3 audio destruction releases an active native stream", "[unit][audio][sdl3]") {
            auto backend = std::move(CreateSdl3AudioBackend(Config())).Value();
            const auto epoch = OpenDefault(*backend);
            Trace trace;
            StartRendering(*backend, epoch, trace);
            backend.reset();

            auto reopened = std::move(CreateSdl3AudioBackend(Config())).Value();
            REQUIRE(std::holds_alternative<AudioBackendProbe>(Complete(*reopened, Probe{}).outcome));
        }
    }  // namespace
}  // namespace Horo::Audio::Backend
