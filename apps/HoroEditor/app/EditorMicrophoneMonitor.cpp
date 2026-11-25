#include "EditorMicrophoneMonitor.h"

#include "EditorAudioPreviewHost.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <new>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        using namespace Audio;
        using namespace Audio::Backend;
        using Clock = std::chrono::steady_clock;
        using ListeningStatus = EditorMicrophoneMonitorStatus;

        constexpr std::uint64_t ClockDomain = 1;
        constexpr std::uint64_t Generation = 1;  // The host assigns each one-shot session an independent runtime identity.
        constexpr auto StartupTimeout = std::chrono::seconds(2);
        constexpr std::uint64_t MaximumQueueAgeNs = 200'000'000;

        /** @brief Excludes migrating or unsupported inputs before showing them in Settings. */
        bool SelectableMonitorInput(const AudioDiscoveredCaptureDevice &device) noexcept {
            return device.deviceClass != AudioDeviceClass::Virtual && device.preferredFormat.has_value();
        }

        /** @brief Keeps the selected input and its advertised processing format in the same discovery revision. */
        Result<AudioCaptureBufferDescriptor> PrepareInput(const AudioCaptureDeviceSnapshot &catalog, const AudioCaptureDeviceId input) {
            using Output = Result<AudioCaptureBufferDescriptor>;
            const auto device = std::ranges::find(catalog.devices, input, &AudioDiscoveredCaptureDevice::id);
            if (device == catalog.devices.end() || !SelectableMonitorInput(*device))
                return Output::Failure(MakeError(AudioErrors::DeviceUnavailable));
            const auto ageFrames = device->preferredFormat->sampleRate * MaximumQueueAgeNs / 1'000'000'000;
            const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(4096, ageFrames));
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

    /** @copydoc EditorMicrophoneMonitor::CanSelectOutput */
    bool EditorMicrophoneMonitor::CanSelectOutput(const AudioDeviceId output) const noexcept {
        return state_->output->CanSelect(output);
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

    EditorMicrophoneMonitorControls::EditorMicrophoneMonitorControls(std::function<bool()> inputAvailable)
        : inputAvailable_(std::move(inputAvailable)) {}

    /** @copydoc EditorMicrophoneMonitorControls::CanConfigure */
    bool EditorMicrophoneMonitorControls::CanConfigure() const noexcept {
        return !OccupiesInput() && inputAvailable_();
    }

    /** @copydoc EditorMicrophoneMonitorControls::OccupiesInput */
    bool EditorMicrophoneMonitorControls::OccupiesInput() const noexcept {
        using enum ListeningStatus;
        return status_ == Scanning || status_ == Starting || status_ == Listening || status_ == Stopping;
    }

    /** @copydoc EditorMicrophoneMonitorControls::Scan */
    void EditorMicrophoneMonitorControls::Scan() noexcept {
        if (CanConfigure())
            status_ = ListeningStatus::Scanning;
    }

    /** @copydoc EditorMicrophoneMonitorControls::InputOptions */
    std::span<const EditorMicrophoneMonitorOption> EditorMicrophoneMonitorControls::InputOptions() const noexcept {
        return inputs_;
    }

    /** @copydoc EditorMicrophoneMonitorControls::OutputOptions */
    std::span<const EditorMicrophoneMonitorOption> EditorMicrophoneMonitorControls::OutputOptions() const noexcept {
        return outputs_;
    }

    /** @copydoc EditorMicrophoneMonitorControls::StartListening */
    void EditorMicrophoneMonitorControls::StartListening(const std::size_t input, const std::size_t output,
                                                           const bool privateListening) noexcept {
        if (!privateListening || status_ != ListeningStatus::Ready || !CanConfigure() || input >= inputs_.size() || output >= outputs_.size())
            return;
        selection_ = Selection{session_->Inputs()[inputs_[input].discoveryIndex].id, session_->Outputs()[outputs_[output].discoveryIndex].id};
        status_ = ListeningStatus::Starting;
    }

    /** @copydoc EditorMicrophoneMonitorControls::StopListening */
    void EditorMicrophoneMonitorControls::StopListening() noexcept {
        if (status_ == ListeningStatus::Starting || status_ == ListeningStatus::Listening) {
            retirementResult_ = ListeningStatus::Idle;
            status_ = ListeningStatus::Stopping;
        }
    }

    /** @copydoc EditorMicrophoneMonitorControls::MonitorStatus */
    EditorMicrophoneMonitorStatus EditorMicrophoneMonitorControls::MonitorStatus() const noexcept {
        return status_;
    }

    /** @brief Builds the Settings choices on control, retaining each original discovery position. */
    void EditorMicrophoneMonitorControls::Discover() {
        Withdraw(ListeningStatus::Failed);
        if (nextRuntime_ < FirstEditorMonitorRuntime)
            return;
        auto discovered = EditorMicrophoneMonitor::Discover(AudioRuntimeId::Create(nextRuntime_--).Value());
        if (discovered.HasError())
            return;
        session_ = std::move(discovered).Value();
        const auto inputs = session_->Inputs();
        for (std::size_t index = 0; index < inputs.size(); ++index)
            if (SelectableMonitorInput(inputs[index]))
                inputs_.emplace_back(index, inputs[index].displayName);
        const auto outputs = session_->Outputs();
        for (std::size_t index = 0; index < outputs.size(); ++index)
            if (session_->CanSelectOutput(outputs[index].id))
                outputs_.emplace_back(index, outputs[index].displayName);
        if (!inputs_.empty() && !outputs_.empty())
            status_ = ListeningStatus::Ready;
        else
            Withdraw(ListeningStatus::Failed);
    }

    /** @brief Transfers the queued endpoint identities to one native session. */
    void EditorMicrophoneMonitorControls::StartSelected() {
        const auto selected = std::exchange(selection_, std::nullopt);
        auto jobs = std::make_shared<JobSystem>(JobSystemConfig{.workerCount = 1});
        if (auto started = session_->Start(selected->input, selected->output, true, std::move(jobs)); started.HasError())
            Withdraw(ListeningStatus::Failed);
    }

    /** @brief Projects native permission loss and other terminal failures into the Settings state. */
    void EditorMicrophoneMonitorControls::Pump() {
        const auto result = session_->Pump(true);
        if (result.HasError()) {
            const auto denied = result.ErrorValue().code.Value() == AudioErrors::CapturePermissionDenied.code.Value();
            Withdraw(denied ? ListeningStatus::PermissionDenied : ListeningStatus::Failed);
        } else if (session_->Phase() == EditorMicrophoneMonitorPhase::Listening) {
            status_ = ListeningStatus::Listening;
        }
    }

    /** @brief Revokes the old selections and releases the session only after output detachment. */
    void EditorMicrophoneMonitorControls::Withdraw(const ListeningStatus result) noexcept {
        selection_.reset();
        inputs_.clear();
        outputs_.clear();
        if (session_) {
            session_->Stop();
            if (session_->Phase() != EditorMicrophoneMonitorPhase::Closed) {
                status_ = ListeningStatus::Stopping;
                retirementResult_ = result;
                return;
            }
            session_.reset();
        }
        status_ = result;
    }

    /** @brief Keeps driving a revoked output until retirement is proven, regardless of Settings focus. */
    void EditorMicrophoneMonitorControls::DriveRetirement() {
        if (session_) {
            session_->Stop();
            static_cast<void>(session_->Pump(false));
            if (session_->Phase() != EditorMicrophoneMonitorPhase::Closed)
                return;
        }
        Withdraw(retirementResult_);
    }

    /** @brief Drives discovery or capture commands only while the Settings scope remains authorized. */
    void EditorMicrophoneMonitorControls::AdvanceForeground() {
        using enum ListeningStatus;
        if (status_ == Scanning)
            Discover();
        else if (selection_)
            StartSelected();
        else if (status_ == Starting || status_ == Listening)
            Pump();
    }

    /** @copydoc EditorMicrophoneMonitorControls::Advance */
    void EditorMicrophoneMonitorControls::Advance(const bool foreground, const bool authorized) noexcept {
        using enum ListeningStatus;
        try {
            if (status_ == Stopping) {
                DriveRetirement();
            } else if (!foreground || !authorized) {
                if (session_ || OccupiesInput())
                    Withdraw(foreground ? PermissionDenied : Idle);
            } else {
                AdvanceForeground();
            }
        } catch (const std::bad_alloc &) {
            Withdraw(Failed);
        } catch (const std::system_error &) {
            Withdraw(Failed);
        }
    }
}  // namespace Horo::Editor
