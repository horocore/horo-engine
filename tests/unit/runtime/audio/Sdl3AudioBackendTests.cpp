#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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
            std::atomic<bool> safetyHooks{};
        };

        RenderResult Render(void *context, const RenderInvocation &invocation) noexcept {
            auto &trace = *static_cast<Trace *>(context);
            trace.calls.fetch_add(1, std::memory_order_relaxed);
            if (trace.safetyHooks.load(std::memory_order_relaxed)) {
                AudioCallbackWatchdog::OnAllocationAttempt();
                AudioCallbackWatchdog::OnLockAttempt();
            }
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

        enum class PlaybackSelection {
            FollowDefault,
            PinPhysical
        };

        AudioDeviceEpoch OpenPlayback(Sdl3AudioBackend &backend, const PlaybackSelection selection) {
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

            const auto physical = std::ranges::find(catalog.devices, AudioDeviceClass::Physical, &AudioDiscoveredDevice::deviceClass);
            if (selection == PlaybackSelection::PinPhysical)
                REQUIRE(physical != catalog.devices.end());
            const auto selected = selection == PlaybackSelection::PinPhysical ? physical->id : catalog.devices.front().id;
            const AudioDeviceEpoch epoch{selected, 1, 1};
            const auto device = selection == PlaybackSelection::PinPhysical ? AudioDeviceSelection{selected}
                                                                            : AudioDeviceSelection{AudioDefaultDeviceRole::Multimedia};
            const AudioDeviceFormatRequest format{.device = device,
                                                  .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                                  .period = {64, 256, 2'048},
                                                  .nativeRatePolicy = AudioDeviceRatePolicy::AllowPreparedResampler};
            const auto opened = std::get<Opened>(Complete(backend, Open{epoch, format, AccessMode::Shared}).outcome);
            REQUIRE(opened.format.device == epoch.device);
            REQUIRE(ValidateAudioDeviceNegotiation(format, catalog, opened.format).status == AudioDeviceNegotiationStatus::Accepted);
            return epoch;
        }

        struct BackendFixture final {
            std::unique_ptr<Sdl3AudioBackend> backend;
            AudioDeviceEpoch epoch;
        };

        BackendFixture CreateOpenedBackend(const PlaybackSelection selection = PlaybackSelection::FollowDefault) {
            auto created = CreateSdl3AudioBackend(Config());
            REQUIRE(created.HasValue());
            auto backend = std::move(created).Value();
            REQUIRE(backend->Kind() == AudioBackendKind::SDL3Audio);
            const auto epoch = OpenPlayback(*backend, selection);
            return {std::move(backend), epoch};
        }

        void StartRendering(Sdl3AudioBackend &backend, const AudioDeviceEpoch &epoch, Trace &trace) {
            REQUIRE(std::holds_alternative<Started>(Complete(backend, Start{epoch, {&trace, Render}}).outcome));
            REQUIRE(WaitForEvent(backend, ExpectedCallbackEvent::Ready));
        }

        /** @brief Pins an explicit SDL dummy microphone without touching a physical device. */
        AudioCaptureBufferDescriptor MonitorCapture(const AudioCaptureDeviceSnapshot &catalog) {
            const auto source = std::ranges::find_if(catalog.devices, [](const auto &device) {
                return device.deviceClass != AudioDeviceClass::Virtual && device.preferredFormat.has_value();
            });
            REQUIRE(source != catalog.devices.end());
            return {.device = source->id,
                    .generation = 1,
                    .clockDomain = Config().clockDomain,
                    .format = *source->preferredFormat,
                    .slots = 2,
                    .maximumFrames = 4096,
                    .budgetBytes = 256 * 1024};
        }

        /** @brief Waits for one bounded dummy callback block without involving output or capture callbacks directly. */
        template <class Read> void RequireMonitorBlock(const AudioCaptureBufferDescriptor &capture, Read readBlock) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            std::vector<AudioSample> pcm(capture.maximumFrames * capture.format.layout.orderedChannels.size());
            while (std::chrono::steady_clock::now() < deadline) {
                const auto read = readBlock(pcm);
                REQUIRE(read.HasValue());
                if (read.Value() == AudioMonitorRead::Copied)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("The local monitor did not receive a current dummy capture block.");
        }

        /** @brief Independently exercised reasons for stopping the bound live source. */
        enum class MonitorRetirement { QuiesceOutput, WithdrawConsent };

        /** @brief Pipes real SDL capture through the sole live stream and proves both routes close together. */
        void VerifyMonitorStream(Sdl3AudioBackend &backend, Sdl3CaptureGrant grant, const AudioCaptureBufferDescriptor &capture,
                                 const AudioDeviceEpoch &epoch, const MonitorRetirement retirement,
                                 const AudioStreamRenderPort &port) {
            AudioCapturedBlock raw;
            CHECK(backend.ReadMonitoring(capture.generation, grant, raw, {}).HasError());
            RequireMonitorBlock(capture, [&](const std::span<AudioSample> pcm) { return backend.PumpMonitoringSource(grant, pcm); });
            CHECK(port.AvailableFrames() > 0);

            if (retirement == MonitorRetirement::QuiesceOutput) {
                static_cast<void>(Begin(backend, Quiesce{epoch}));
                REQUIRE(backend.AdvanceControl().HasValue());
            } else {
                grant.foreground = false;
                CHECK(backend.PumpMonitoringSource(grant, {}).HasError());
            }
            CHECK(backend.CaptureActivity().state == Sdl3CaptureActivityState::Inactive);
            CHECK(port.IsStopped());
            CHECK(port.AvailableFrames() == 0);
            CHECK(backend.PumpMonitoringSource(grant, {}).HasError());
        }

        /** @brief Rejects a playback reservation beyond the monitor's queue budget without adopting it. */
        void VerifyMonitorBinding(Sdl3AudioBackend &backend, const Sdl3CaptureGrant &grant, const AudioCaptureBufferDescriptor &capture,
                                  const std::shared_ptr<AudioStreamingService> &service, const AudioStreamHandle prepared) {
            const auto large = service->AdmitLivePcm({capture.format, MaximumAudioCallbackFrames * 64});
            REQUIRE(large.HasValue());
            CHECK(backend.BindMonitoringSource(grant, service, large.Value()).HasError());
            REQUIRE(service->Retire(large.Value()).HasValue());
            REQUIRE(backend.BindMonitoringSource(grant, service, prepared).HasValue());
            CHECK(backend.BindMonitoringSource(grant, service, prepared).HasError());
        }

        /** @brief Describes the fixture's confirmed private route without granting it to any other output. */
        Sdl3CaptureGrant MonitorGrant(const AudioCaptureBufferDescriptor &capture, const std::uint64_t discoveryRevision,
                                      const AudioDeviceEpoch &output) {
            const AudioCaptureMonitorRoute route{.input = capture.device,
                                                 .captureGeneration = capture.generation,
                                                 .output = output,
                                                 .bus = {Owner(), 3, 1},
                                                 .clockDomain = capture.clockDomain,
                                                 .maximumAgeNanoseconds = 200'000'000,
                                                 .gain = 0.5F,
                                                 .isolation = AudioMonitorIsolation::PrivateListening};
            return {.device = capture.device,
                    .discoveryRevision = discoveryRevision,
                    .generation = capture.generation,
                    .authorityRevision = 1,
                    .intent = {.purpose = Sdl3CapturePurpose::Monitoring, .monitor = route},
                    .foreground = true};
        }

        TEST_CASE("SDL local monitoring requires a pinned output and retires before output quiescence", "[audio][sdl3][monitor]") {
            auto selection = PlaybackSelection::FollowDefault;
            auto retirement = MonitorRetirement::QuiesceOutput;
            SECTION("automatically migrating system output cannot be monitored") {
                selection = PlaybackSelection::FollowDefault;
            }
            SECTION("pinned physical output has an exact local route") {
                selection = PlaybackSelection::PinPhysical;
            }
            SECTION("consent withdrawal retires the pinned route") {
                selection = PlaybackSelection::PinPhysical;
                retirement = MonitorRetirement::WithdrawConsent;
            }
            Trace trace;
            auto fixture = CreateOpenedBackend(selection);
            auto &backend = *fixture.backend;
            const auto discovered = backend.DiscoverCapture();
            REQUIRE(discovered.HasValue());
            const auto &catalog = discovered.Value().snapshot;
            const auto capture = MonitorCapture(catalog);
            JobSystem jobs({.workerCount = 1});
            auto live = AudioStreamingService::CreateLive(jobs);
            REQUIRE(live.HasValue());
            std::shared_ptr<AudioStreamingService> service{std::move(live).Value()};
            const auto prepared = service->AdmitLivePcm({capture.format, capture.maximumFrames});
            REQUIRE(prepared.HasValue());
            auto port = service->RenderPort(prepared.Value());
            REQUIRE(port.HasValue());
            CHECK(port.Value().AvailableFrames() == 0);
            StartRendering(backend, fixture.epoch, trace);
            REQUIRE(backend.CommitRendering(fixture.epoch).HasValue());
            const auto grant = MonitorGrant(capture, catalog.revision, fixture.epoch);
            auto unproven = grant;
            unproven.intent.monitor.reset();
            CHECK(backend.StartCapture(capture, unproven.intent, unproven).HasError());
            const auto started = backend.StartCapture(capture, grant.intent, grant);
            if (selection == PlaybackSelection::FollowDefault) {
                CHECK(started.HasError());
                return;
            }
            REQUIRE(started.HasValue());
            AudioCapturedBlock raw;
            CHECK(backend.ReadCapture(capture.generation, grant, raw, {}).HasError());
            RequireMonitorBlock(capture, [&](const std::span<AudioSample> pcm) {
                return backend.ReadMonitoring(capture.generation, grant, raw, pcm);
            });
            VerifyMonitorBinding(backend, grant, capture, service, prepared.Value());
            VerifyMonitorStream(backend, grant, capture, fixture.epoch, retirement, port.Value());
        }

        TEST_CASE("SDL3 audio drives the Horo render port and detaches before close", "[unit][audio][sdl3]") {
            auto fixture = CreateOpenedBackend();
            auto &backend = fixture.backend;
            const auto epoch = fixture.epoch;
            Trace trace;
            StartRendering(*backend, epoch, trace);
            REQUIRE(backend->CommitRendering(epoch).HasValue());
            REQUIRE(trace.calls.load(std::memory_order_relaxed) > 0);
            trace.safetyHooks.store(true, std::memory_order_relaxed);
            std::array<AudioCallbackViolation, 4> safety{};
            AudioCallbackViolationDrain safetyDrain;
#if !defined(NDEBUG)
            const auto safetyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (safetyDrain.count == 0 && std::chrono::steady_clock::now() < safetyDeadline) {
                safetyDrain = backend->DrainSafetyViolations(safety);
                std::this_thread::yield();
            }
            REQUIRE(safetyDrain.count > 0);
            CHECK(safety[0].epoch == epoch);
#else
            safetyDrain = backend->DrainSafetyViolations(safety);
            REQUIRE(safetyDrain.count == 0);
#endif
            trace.safetyHooks.store(false, std::memory_order_relaxed);
            trace.invalidResponse.store(true, std::memory_order_relaxed);
            REQUIRE(WaitForEvent(*backend, ExpectedCallbackEvent::Fault));
            trace.invalidResponse.store(false, std::memory_order_relaxed);

            const auto quiesce = Begin(*backend, Quiesce{epoch});
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(WaitForEvent(*backend, ExpectedCallbackEvent::Quiesced));
            REQUIRE(backend->AdvanceControl().HasValue());
            const auto quiesced = backend->Poll(quiesce);
            REQUIRE(quiesced.HasValue());
            REQUIRE(quiesced.Value());
            REQUIRE(std::get_if<Quiesced>(&quiesced.Value()->outcome));
            REQUIRE(backend->AcknowledgeCompletion(quiesce).HasValue());
            const auto stopped = Complete(*backend, Stop{epoch});
            REQUIRE(std::get_if<Stopped>(&stopped.outcome));
            REQUIRE(std::holds_alternative<Closed>(Complete(*backend, Close{}).outcome));
        }

        TEST_CASE("SDL3 audio retires attached priming without committing rendering", "[unit][audio][sdl3]") {
            auto fixture = CreateOpenedBackend();
            auto &backend = *fixture.backend;
            const auto epoch = fixture.epoch;
            Trace trace;
            REQUIRE(std::holds_alternative<Started>(Complete(backend, Start{epoch, {&trace, Render}}).outcome));
            const auto quiesce = Begin(backend, Quiesce{epoch});
            REQUIRE(backend.AdvanceControl().HasValue());
            CHECK(backend.CommitRendering(epoch).HasError());
            REQUIRE(WaitForEvent(backend, ExpectedCallbackEvent::Quiesced));
            REQUIRE(backend.AdvanceControl().HasValue());
            const auto completion = backend.Poll(quiesce);
            REQUIRE(completion.HasValue());
            REQUIRE(completion.Value().has_value());
            REQUIRE(std::get<Quiesced>(completion.Value()->outcome).epoch == epoch);
            REQUIRE(backend.AcknowledgeCompletion(quiesce).HasValue());
            REQUIRE(std::get<Stopped>(Complete(backend, Stop{epoch}).outcome).epoch == epoch);
            REQUIRE(std::holds_alternative<Closed>(Complete(backend, Close{}).outcome));
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
            auto fixture = CreateOpenedBackend();
            Trace trace;
            StartRendering(*fixture.backend, fixture.epoch, trace);
            fixture.backend.reset();

            auto reopened = std::move(CreateSdl3AudioBackend(Config())).Value();
            REQUIRE(std::holds_alternative<AudioBackendProbe>(Complete(*reopened, Probe{}).outcome));
        }

        TEST_CASE("SDL3 discovers native recording endpoints independently of playback", "[unit][audio][sdl3][capture]") {
            auto backend = std::move(CreateSdl3AudioBackend(Config())).Value();
            AudioBackend &capture = *backend;
            const auto first = capture.DiscoverCapture();
            REQUIRE(first.HasValue());
            const auto &catalog = first.Value().snapshot;
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(catalog));
            REQUIRE_FALSE(catalog.devices.empty());
            REQUIRE(catalog.devices.front().preferredFormat);
            REQUIRE(first.Value().changes.size() == catalog.devices.size());
            CHECK(catalog.owner == Owner());
            CHECK(catalog.permission == AudioCapturePermission::NotRequired);
            const auto system = ResolveAudioCaptureDevice(catalog, AudioDefaultDeviceRole::Multimedia);
            REQUIRE(system.status == AudioDeviceResolutionStatus::Resolved);
            REQUIRE(first.Value().defaultChanges[1]);
            CHECK(first.Value().defaultChanges[1]->role == AudioDefaultDeviceRole::Multimedia);
            CHECK_FALSE(first.Value().defaultChanges[1]->previous);
            CHECK(first.Value().defaultChanges[1]->current == system.device);
            REQUIRE(first.Value().permissionChange);
            CHECK(first.Value().permissionChange->previous == AudioCapturePermission::Unknown);
            CHECK(first.Value().permissionChange->current == AudioCapturePermission::NotRequired);
            const auto endpoint = std::ranges::find(catalog.devices, system.device, &AudioDiscoveredCaptureDevice::id);
            REQUIRE(endpoint != catalog.devices.end());
            CHECK(endpoint->deviceClass == AudioDeviceClass::Unknown);
            CHECK(endpoint->preferredFormat.has_value());
            CHECK(std::ranges::none_of(catalog.devices, [](const AudioDiscoveredCaptureDevice &device) {
                return device.deviceClass == AudioDeviceClass::Virtual;
            }));
            CHECK(ResolveAudioCaptureDevice(catalog, AudioDefaultDeviceRole::Communications).status ==
                  AudioDeviceResolutionStatus::Unavailable);
            CHECK(backend->State() == Sdl3AudioBackendState::Closed);
            for (const auto &change : first.Value().changes) {
                CHECK(change.kind == AudioCaptureDeviceChangeKind::Added);
                CHECK(change.revision == catalog.revision);
                CHECK(ResolveAudioCaptureDevice(catalog, change.device).status == AudioDeviceResolutionStatus::Resolved);
            }

            const auto playback = Complete(*backend, Enumerate{});
            CHECK(std::holds_alternative<AudioDeviceSnapshot>(playback.outcome));
            const auto again = capture.DiscoverCapture();
            REQUIRE(again.HasValue());
            CHECK(again.Value().snapshot.devices == catalog.devices);
            CHECK(again.Value().snapshot.revision == catalog.revision);
            CHECK(again.Value().snapshot.defaults.multimedia == system.device);
            CHECK(again.Value().changes.empty());
            CHECK_FALSE(again.Value().defaultChanges[1]);
            CHECK_FALSE(again.Value().permissionChange);
            CHECK(backend->State() == Sdl3AudioBackendState::Closed);
        }

#if defined(__APPLE__) && TARGET_OS_OSX
        TEST_CASE("SDL3 Core Audio discovery observes microphone authorization", "[.native][audio][sdl3][capture]") {
            auto backend = std::move(CreateSdl3AudioBackend(Config())).Value();
            const auto discovered = backend->DiscoverCapture();
            REQUIRE(discovered.HasValue());
            const auto &snapshot = discovered.Value().snapshot;
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(snapshot));
            CHECK(snapshot.permission != AudioCapturePermission::Unknown);
            CHECK(snapshot.permission != AudioCapturePermission::NotRequired);
            REQUIRE(discovered.Value().permissionChange);
            CHECK(discovered.Value().permissionChange->previous == AudioCapturePermission::Unknown);
            CHECK(discovered.Value().permissionChange->current == snapshot.permission);
            CHECK(backend->State() == Sdl3AudioBackendState::Closed);
        }

        TEST_CASE("SDL3 Core Audio resolves a native system recording default", "[.native][audio][sdl3][capture]") {
            auto backend = std::move(CreateSdl3AudioBackend(Config())).Value();
            const auto discovered = backend->DiscoverCapture();
            REQUIRE(discovered.HasValue());
            const auto &snapshot = discovered.Value().snapshot;
            const auto selected = ResolveAudioCaptureDevice(snapshot, AudioDefaultDeviceRole::Multimedia);
            REQUIRE(selected.status == AudioDeviceResolutionStatus::Resolved);
            const auto endpoint = std::ranges::find(snapshot.devices, selected.device, &AudioDiscoveredCaptureDevice::id);
            REQUIRE(endpoint != snapshot.devices.end());
            CHECK(endpoint->deviceClass == AudioDeviceClass::Unknown);
            CHECK(endpoint->preferredFormat.has_value());
            CHECK(backend->State() == Sdl3AudioBackendState::Closed);
        }
#endif
    }  // namespace
}  // namespace Horo::Audio::Backend
