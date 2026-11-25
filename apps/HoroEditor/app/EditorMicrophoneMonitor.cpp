#include "EditorMicrophoneMonitor.h"

#include "EditorAudioPreviewHost.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <algorithm>
#include <chrono>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Audio;
        using namespace Audio::Backend;
        using Clock = std::chrono::steady_clock;

        constexpr std::uint64_t ClockDomain = 1;
        constexpr std::uint64_t Generation = 1;  // The host assigns each one-shot session an independent runtime identity.
        constexpr auto StartupTimeout = std::chrono::seconds(2);
        constexpr std::uint64_t MaximumQueueAgeNs = 200'000'000;

        /** @brief Keeps the selected input and its advertised processing format in the same discovery revision. */
        Result<AudioCaptureBufferDescriptor> PrepareInput(const AudioCaptureDeviceSnapshot &catalog, const AudioCaptureDeviceId input) {
            using Output = Result<AudioCaptureBufferDescriptor>;
            const auto device = std::ranges::find(catalog.devices, input, &AudioDiscoveredCaptureDevice::id);
            if (device == catalog.devices.end() || device->deviceClass == AudioDeviceClass::Virtual || !device->preferredFormat)
                return Output::Failure(MakeError(AudioErrors::DeviceUnavailable));
            const auto frames = std::min<std::uint32_t>(4096, device->preferredFormat->sampleRate / 5);
            return Output::Success({input, Generation, ClockDomain, *device->preferredFormat, 4, frames, 256U << 10U});
        }
    }  // namespace

    struct EditorMicrophoneMonitor::State final {
        std::unique_ptr<EditorAudioMonitorOutput> output;
        Sdl3AudioBackend *capture{};  // Borrows the single SDL peer retained by output.
        AudioCaptureDeviceSnapshot inputs;
        std::shared_ptr<JobSystem> jobs;  // Retains the service dependency until after detached playback retirement.
        std::shared_ptr<AudioStreamingService> service;
        AudioStreamHandle stream;
        std::vector<AudioSample> scratch;
        AudioCaptureBufferDescriptor input;
        AudioBusHandle bus;
        std::optional<Sdl3CaptureGrant> grant;
        Clock::time_point startupDeadline;
        EditorMicrophoneMonitorPhase phase{EditorMicrophoneMonitorPhase::Ready};

        /** @brief Revalidates the exact input and binds the prepared voice only after the pinned output is active. */
        Result<void> Activate(const AudioDeviceEpoch &epoch) {
            auto current = capture->DiscoverCapture();
            if (current.HasError())
                return Result<void>::Failure(std::move(current).ErrorValue());
            const AudioCaptureMonitorRoute route{input.device, input.generation, epoch, bus, input.clockDomain,
                                                 MaximumQueueAgeNs, 0.5F, AudioMonitorIsolation::PrivateListening};
            const Sdl3CaptureGrant admission{.device = input.device,
                                             .discoveryRevision = current.Value().snapshot.revision,
                                             .generation = input.generation,
                                             .authorityRevision = Generation,
                                             .intent = {.purpose = Sdl3CapturePurpose::Monitoring, .monitor = route},
                                             .foreground = true};
            if (auto opened = capture->StartCapture(input, admission.intent, admission); opened.HasError())
                return opened;
            grant = admission;
            if (auto bound = capture->BindMonitoringSource(admission, service, stream); bound.HasError())
                return bound;
            return output->StartVoice();
        }

        /** @brief Supplies one gain-limited current block and treats queue pressure as a drop. */
        Result<void> CopyMicrophoneBlock() {
            auto copied = capture->PumpMonitoringSource(*grant, scratch);
            if (copied.HasError()) {
                if (copied.ErrorValue().code.Value() == AudioErrors::StreamCapacityExceeded.code.Value())
                    return Result<void>::Success();
                return Result<void>::Failure(std::move(copied).ErrorValue());
            }
            if (copied.Value() == AudioMonitorRead::Cancelled || copied.Value() == AudioMonitorRead::InsufficientSpace)
                return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
            return Result<void>::Success();
        }

        /** @brief Waits for exact rendering admission, then attaches the capture side. */
        Result<void> WhileOpening(const AudioFrontendSnapshot &snapshot) {
            if (snapshot.phase == AudioFrontendPhase::Active) {
                auto started = Activate(snapshot.device);
                if (started.HasValue())
                    phase = EditorMicrophoneMonitorPhase::Listening;
                return started;
            }
            if (Clock::now() >= startupDeadline)
                return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
            return Result<void>::Success();
        }

        /** @brief Advances only the current part of the route, keeping detach independent of consent. */
        Result<void> Advance() {
            using enum EditorMicrophoneMonitorPhase;
            auto advanced = output->Pump();
            const auto snapshot = output->Snapshot();
            if (phase == Closing) {
                if (snapshot.phase == AudioFrontendPhase::Closed || snapshot.phase == AudioFrontendPhase::Prepared)
                    phase = Closed;
                return advanced;
            }
            if (advanced.HasError())
                return advanced;
            return phase == Listening ? CopyMicrophoneBlock() : WhileOpening(snapshot);
        }
    };

    EditorMicrophoneMonitor::EditorMicrophoneMonitor(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    EditorMicrophoneMonitor::~EditorMicrophoneMonitor() {
        Stop();
    }

    /** @copydoc EditorMicrophoneMonitor::Discover */
    Result<std::unique_ptr<EditorMicrophoneMonitor>> EditorMicrophoneMonitor::Discover(const AudioRuntimeId owner) {
        using Output = Result<std::unique_ptr<EditorMicrophoneMonitor>>;
        try {
            auto native = CreateSdl3AudioBackend({owner, ClockDomain});
            if (native.HasError())
                return Output::Failure(std::move(native).ErrorValue());
            auto *capture = native.Value().get();
            auto output = EditorAudioMonitorOutput::Discover({std::move(native).Value(), [capture] { return capture->AdvanceControl(); }});
            if (output.HasError())
                return Output::Failure(std::move(output).ErrorValue());
            auto inputs = capture->DiscoverCapture();
            if (inputs.HasError())
                return Output::Failure(std::move(inputs).ErrorValue());
            auto state = std::make_unique<State>();
            state->output = std::move(output).Value();
            state->capture = capture;
            state->inputs = std::move(inputs).Value().snapshot;
            return Output::Success(std::unique_ptr<EditorMicrophoneMonitor>{new EditorMicrophoneMonitor(std::move(state))});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc EditorMicrophoneMonitor::Inputs */
    std::span<const AudioDiscoveredCaptureDevice> EditorMicrophoneMonitor::Inputs() const noexcept {
        return state_->inputs.devices;
    }

    /** @copydoc EditorMicrophoneMonitor::Outputs */
    std::span<const AudioDiscoveredDevice> EditorMicrophoneMonitor::Outputs() const noexcept {
        return state_->output->Outputs();
    }

    /** @copydoc EditorMicrophoneMonitor::Start */
    Result<void> EditorMicrophoneMonitor::Start(const AudioCaptureDeviceId input, const AudioDeviceId output, const bool privateListening,
                                               std::shared_ptr<JobSystem> jobs) {
        using enum EditorMicrophoneMonitorPhase;
        if (Phase() != Ready || !jobs)
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!privateListening)
            return Result<void>::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        if (!state_->output->CanSelect(output))
            return Result<void>::Failure(MakeError(AudioErrors::DeviceUnavailable));
        try {
            auto source = PrepareInput(state_->inputs, input);
            if (source.HasError())
                return Result<void>::Failure(std::move(source).ErrorValue());
            auto live = AudioStreamingService::CreateLive(*jobs);
            if (live.HasError())
                return Result<void>::Failure(std::move(live).ErrorValue());
            std::shared_ptr<AudioStreamingService> service{std::move(live).Value()};
            auto stream = service->AdmitLivePcm({source.Value().format, source.Value().maximumFrames});
            if (stream.HasError())
                return Result<void>::Failure(std::move(stream).ErrorValue());
            std::vector<AudioSample> scratch(source.Value().maximumFrames * source.Value().format.layout.orderedChannels.size());
            state_->phase = Opening;
            auto bus = state_->output->Start(output, service, stream.Value(), jobs);
            if (bus.HasError()) {
                Stop();
                return Result<void>::Failure(std::move(bus).ErrorValue());
            }
            state_->jobs = std::move(jobs);
            state_->service = std::move(service);
            state_->stream = stream.Value();
            state_->scratch = std::move(scratch);
            state_->input = std::move(source).Value();
            state_->bus = bus.Value();
            state_->startupDeadline = Clock::now() + StartupTimeout;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            Stop();
            return Result<void>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc EditorMicrophoneMonitor::Pump */
    Result<void> EditorMicrophoneMonitor::Pump(const bool authorized) {
        using enum EditorMicrophoneMonitorPhase;
        if (Phase() == Ready || Phase() == Closed)
            return Result<void>::Success();
        if (Phase() != Closing && !authorized) {
            Stop();
            return Result<void>::Failure(MakeError(AudioErrors::CapturePermissionDenied));
        }
        auto advanced = state_->Advance();
        if (advanced.HasError())
            Stop();
        return advanced;
    }

    /** @copydoc EditorMicrophoneMonitor::Stop */
    void EditorMicrophoneMonitor::Stop() noexcept {
        using enum EditorMicrophoneMonitorPhase;
        if (Phase() == Closing || Phase() == Closed)
            return;
        if (state_->grant) {
            static_cast<void>(state_->capture->StopCapture(state_->grant->generation));
            state_->grant.reset();
        }
        state_->output->Close();
        state_->phase = Phase() == Ready ? Closed : Closing;
    }

    /** @copydoc EditorMicrophoneMonitor::Phase */
    EditorMicrophoneMonitorPhase EditorMicrophoneMonitor::Phase() const noexcept {
        return state_->phase;
    }
}  // namespace Horo::Editor
