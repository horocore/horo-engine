#include "EditorMicrophonePermission.h"

#include "EditorMicrophoneMonitor.h"
#include "EditorMicrophoneRecording.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Security/SecureMemory.h"
#include "ImmersiveSpeechTranscription.h"
#include "MacOnDeviceSpeechProvider.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Editor {
    const ErrorCodeDescriptor EditorSpeechCaptureDurationLimit{ErrorDomainId{"horo.editor.speech_capture"},
                                                               ErrorCode{"horo.editor.speech_capture.duration_limit"},
                                                               ErrorSeverity::Warning, "The speech recording reached its duration limit.",
                                                               "Start a shorter recording."};

    namespace {
        /** @brief Projects typed capture and storage failures into safe settings messages. */
        EditorMicrophoneRecordingStatus RecordingFailureStatus(const Error &error) noexcept {
            using enum EditorMicrophoneRecordingStatus;
            const auto code = error.code.Value();
            if (code == Audio::AudioErrors::CapturePermissionDenied.code.Value())
                return PermissionDenied;
            if (code == Audio::AudioErrors::CaptureDeviceLost.code.Value() || code == Audio::AudioErrors::DeviceUnavailable.code.Value())
                return DeviceLost;
            if (code == FileStorageErrors::Capacity.code.Value())
                return InsufficientSpace;
            if (code == Audio::AudioErrors::OperationCancelled.code.Value() || code == Assets::AssetSourceErrors::Cancelled.code.Value())
                return Idle;
            return Failed;
        }

        /** @brief Requires foreground admission and both independent native permissions. */
        bool SpeechCaptureAllowed(const bool foreground) noexcept {
            return foreground && Audio::Backend::ObserveSdl3MacCapturePermission().permission == Audio::AudioCapturePermission::Granted &&
                   ObserveMacSpeechAuthorization() == MacSpeechAuthorization::Granted;
        }

    }  // namespace

    /** @brief Owns the shared microphone connection check and private foreground recorder. */
    struct EditorMicrophonePermission::CaptureCheck final {
        using Clock = std::chrono::steady_clock;
        static constexpr Audio::Backend::Sdl3CaptureIntent Intent{.purpose = Audio::Backend::Sdl3CapturePurpose::ConnectionTest};
        static constexpr std::uint64_t ClockDomain = 1;
        static constexpr auto TestDuration = std::chrono::seconds(5);
        static constexpr auto HealthInterval = std::chrono::milliseconds(100);

        std::unique_ptr<Audio::Backend::Sdl3AudioBackend> backend;
        std::vector<std::string> labels;
        std::vector<Audio::AudioCaptureDeviceId> inputs;
        std::optional<Audio::AudioCaptureDeviceId> pending;
        bool scanning{};
        bool stopping{};
        bool saving{};
        std::optional<Audio::Backend::Sdl3CaptureGrant> grant;

        /** @brief The retained editor generation and stop authority survive native detachment together. */
        struct SpeechAdmission final {
            std::stop_source cancellation;
            ImmersivePlanningContext context;
        };

        std::optional<SpeechAdmission> speech;
        std::uint64_t generation{};
        std::uint64_t expiredSpeechGeneration{};
        Clock::time_point deadline{};
        Clock::time_point nextHealth{};
        EditorMicrophoneTestStatus status{EditorMicrophoneTestStatus::Idle};
        float peakLevel{};
        std::filesystem::path project;
        std::optional<Application::Internal::AudioRecordingDestination> recordingTarget;
        std::optional<EditorRecordingTake> take;
        EditorRecordingPublication publication;
        EditorMicrophoneRecordingStatus recordingStatus{EditorMicrophoneRecordingStatus::Idle};
        EditorMicrophoneMonitorControls monitor{[this] { return !NativeInputBusy(); }};

        struct SavedRecording final {
            std::string path;
            std::optional<Assets::AssetId> asset;
        };

        SavedRecording savedRecording;

        CaptureCheck(ProjectMutationCoordinator &mutations, DurableFileSystem &files, Assets::AssetRegistry &registry)
            : publication(mutations, files, registry) {}

        /** @brief Returns pending or active private PCM ownership, separate from detached publication. */
        [[nodiscard]] bool RecordingRequested() const noexcept {
            return recordingTarget.has_value() || take.has_value();
        }

        /** @brief A live or sealed speech lease reserves the microphone owner until the host retires it. */
        [[nodiscard]] bool InputBusy() const noexcept {
            return NativeInputBusy() || monitor.OccupiesInput();
        }

        /** @brief Reports whether the shared input is reserved by a test, recording or speech. */
        [[nodiscard]] bool NativeInputBusy() const noexcept {
            return grant || pending || scanning || speech;
        }

        /** @brief Revalidates native consent on the same owner tick that processes listening commands. */
        void AdvanceMonitor(const bool foreground) noexcept {
            const bool authorized = foreground && monitor.MonitorStatus() != EditorMicrophoneMonitorStatus::Idle &&
                                    Audio::Backend::ObserveSdl3MacCapturePermission().permission == Audio::AudioCapturePermission::Granted;
            monitor.Advance(foreground, authorized);
        }

        /** @brief Tests the exact generation, including a detached lease awaiting worker completion. */
        [[nodiscard]] bool OwnsSpeech(const std::uint64_t expected) const noexcept {
            return speech && generation == expected;
        }

        /** @brief Rejects stale editor scope and expires a live microphone independently of worker polling. */
        [[nodiscard]] bool SpeechCurrent(const Clock::time_point now) const noexcept {
            return speech && speech->context.IsCurrent() && !SpeechExpired(now);
        }

        /** @brief The live capture deadline does not withdraw an already detached input. */
        [[nodiscard]] bool SpeechExpired(const Clock::time_point now) const noexcept {
            return grant && now >= deadline;
        }

        /** @brief Releases expired PCM immediately while preserving its reason for the exact caller. */
        void ExpireSpeech() noexcept {
            expiredSpeechGeneration = generation;
            Stop();
        }

        /** @brief Creates an inert capture-only backend when the user first requests discovery. */
        bool EnsureBackend() {
            if (backend)
                return true;
            // The playback preview owner reserves the terminal runtime identity and never issues it.
            const auto owner = Audio::AudioRuntimeId::Create(std::numeric_limits<std::uint64_t>::max()).Value();
            auto created = Audio::Backend::CreateSdl3AudioBackend({owner, ClockDomain});
            if (created.HasError())
                return false;
            backend = std::move(created).Value();
            return true;
        }

        /** @brief Projects supported explicit recording endpoints from one discovery revision. */
        bool Discover() {
            if (!EnsureBackend())
                return false;
            const auto discovery = backend->DiscoverCapture();
            if (discovery.HasError())
                return false;
            std::vector<std::string> newLabels;
            std::vector<Audio::AudioCaptureDeviceId> newInputs;
            for (const auto &device : discovery.Value().snapshot.devices) {
                if (device.deviceClass == Audio::AudioDeviceClass::Virtual || !device.preferredFormat || device.displayName.empty())
                    continue;
                newLabels.push_back(device.displayName);
                newInputs.push_back(device.id);
            }
            labels = std::move(newLabels);
            inputs = std::move(newInputs);
            return !inputs.empty();
        }

        /** @brief An endpoint's format and admission revision captured together. */
        struct InputFormat final {
            Audio::AudioProcessingFormat format;
            std::uint64_t revision{};
        };

        /** @brief Refreshes the exact selected endpoint without resolving another default. */
        Result<InputFormat> RefreshSelected(const Audio::AudioCaptureDeviceId selected) {
            using Output = Result<InputFormat>;
            if (!backend)
                return Output::Failure(MakeError(Audio::AudioErrors::DeviceUnavailable));
            const auto discovery = backend->DiscoverCapture();
            if (discovery.HasError())
                return Output::Failure(discovery.ErrorValue());
            const auto &snapshot = discovery.Value().snapshot;
            const auto device = std::ranges::find(snapshot.devices, selected, &Audio::AudioDiscoveredCaptureDevice::id);
            if (device == snapshot.devices.end() || !device->preferredFormat)
                return Output::Failure(MakeError(Audio::AudioErrors::DeviceUnavailable));
            return Output::Success({*device->preferredFormat, snapshot.revision});
        }

        /** @brief Refreshes and opens a foreground-only connection test or recording. */
        Result<void> Start(const Audio::AudioCaptureDeviceId selected, const Clock::time_point now) {
            if (generation == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(Audio::AudioErrors::HandleGenerationExhausted));
            const auto input = RefreshSelected(selected);
            if (input.HasError())
                return Result<void>::Failure(input.ErrorValue());
            return Open(selected, input.Value().revision, input.Value().format, now);
        }

        /** @brief Prepares a revocation token and starts speech on the same exclusive native owner. */
        Result<EditorSpeechCaptureLease> OpenSpeech(const Audio::AudioCaptureDeviceId selected, InputFormat input,
                                                    const ImmersivePlanningContext &context) {
            using Output = Result<EditorSpeechCaptureLease>;
            if (generation == std::numeric_limits<std::uint64_t>::max())
                return Output::Failure(MakeError(Audio::AudioErrors::HandleGenerationExhausted));
            ++generation;
            const Audio::Backend::Sdl3CaptureGrant admission{.device = selected,
                                                             .discoveryRevision = input.revision,
                                                             .generation = generation,
                                                             .authorityRevision = generation,
                                                             .intent = {.purpose = Audio::Backend::Sdl3CapturePurpose::Speech},
                                                             .foreground = true};
            std::stop_source cancellation;
            auto result = Output::Success({{.device = selected,
                                            .generation = generation,
                                            .clockDomain = ClockDomain,
                                            .format = std::move(input.format),
                                            .slots = 8,
                                            .maximumFrames = 4096,
                                            .budgetBytes = 512U << 10U},
                                           cancellation.get_token()});
            if (const auto opened = backend->StartCapture(result.Value().source, admission.intent, admission); opened.HasError())
                return Output::Failure(opened.ErrorValue());
            grant = admission;
            speech.emplace(std::move(cancellation), context);
            deadline = Clock::now() + ImmersiveSpeechTranscription::MaximumInputDuration;
            return result;
        }

        /** @brief Prepares private recording storage before opening the exact admitted native stream. */
        Result<void> Open(const Audio::AudioCaptureDeviceId selected, const std::uint64_t revision,
                          const Audio::AudioProcessingFormat &format, const Clock::time_point now) {
            ++generation;
            const bool recording = recordingTarget.has_value();
            const Audio::Backend::Sdl3CaptureIntent intent{.purpose =
                                                               recording ? Audio::Backend::Sdl3CapturePurpose::Recording : Intent.purpose};
            const Audio::Backend::AudioCaptureBufferDescriptor request{.device = selected,
                                                                       .generation = generation,
                                                                       .clockDomain = ClockDomain,
                                                                       .format = format,
                                                                       .slots = recording ? 16U : 2U,
                                                                       .maximumFrames = 4096,
                                                                       .budgetBytes = recording ? 2U << 20U : 128U << 10U};
            const Audio::Backend::Sdl3CaptureGrant selectedGrant{.device = selected,
                                                                 .discoveryRevision = revision,
                                                                 .generation = generation,
                                                                 .authorityRevision = generation,
                                                                 .intent = intent,
                                                                 .foreground = true};
            if (recording) {
                auto prepared = EditorRecordingTake::Prepare(request, std::move(*recordingTarget));
                recordingTarget.reset();
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                take.emplace(std::move(prepared).Value());
            }
            if (auto opened = backend->StartCapture(request, intent, selectedGrant); opened.HasError())
                return opened;
            grant = selectedGrant;
            deadline = now + (recording ? std::chrono::seconds(20) : TestDuration);
            nextHealth = now;
            return Result<void>::Success();
        }

        /** @brief Cancels queued operations and detaches the current session before presentation clears. */
        void Stop() noexcept {
            scanning = false;
            stopping = false;
            saving = false;
            pending.reset();
            if (speech)
                speech->cancellation.request_stop();
            if (grant)
                static_cast<void>(backend->StopCapture(grant->generation));
            grant.reset();
            speech.reset();
            status = EditorMicrophoneTestStatus::Idle;
            peakLevel = 0.0F;
            recordingTarget.reset();
            take.reset();
        }

        /** @brief Retires a failed take without writing any partial asset. */
        void FailRecording(const EditorMicrophoneRecordingStatus reason = EditorMicrophoneRecordingStatus::Failed) noexcept {
            Stop();
            recordingStatus = reason;
        }

        /** @brief Drains a bounded number of complete microphone blocks per editor update. */
        Result<void> DrainTake() {
            using enum Audio::Backend::AudioCaptureRead;
            for (unsigned i = 0; i < 4; ++i) {
                Audio::Backend::AudioCapturedBlock block;
                const auto read = backend->ReadCapture(grant->generation, grant, block, take->scratch);
                if (read.HasError())
                    return Result<void>::Failure(read.ErrorValue());
                if (read.Value().droppedFrames != 0)
                    return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
                if (read.Value().status == Empty)
                    break;
                if (read.Value().status != Copied)
                    return Result<void>::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
                if (auto appended = take->Append(block); appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        /** @brief Detaches an explicitly saved take and transfers publication off the editor lane. */
        void FinishTake() {
            auto tail = backend->FinishRecording(grant->generation, *grant);
            if (tail.HasError()) {
                FailRecording(RecordingFailureStatus(tail.ErrorValue()));
                return;
            }
            if (!publication.Start(std::move(*take), std::move(tail).Value())) {
                FailRecording();
                return;
            }
            Stop();
            recordingStatus = EditorMicrophoneRecordingStatus::Saving;
        }

        /** @brief Adopts a save result while the project that requested it is still current. */
        void PollPublication() {
            using enum EditorMicrophoneRecordingStatus;
            auto result = publication.Poll();
            if (!result || recordingStatus != Saving)
                return;
            if (result->HasError()) {
                recordingStatus = RecordingFailureStatus(result->ErrorValue());
                return;
            }
            auto saved = std::move(*result).Value();
            savedRecording.path = std::move(saved.path);
            savedRecording.asset = saved.id;
            recordingStatus = Saved;
        }

        /** @brief Reconciles native health periodically and publishes callback activity without reading audio. */
        void Observe(const Clock::time_point now) {
            if (now >= nextHealth) {
                nextHealth = now + HealthInterval;
                if (const auto current = backend->ReconcileCapture(grant->generation, grant); current.HasError()) {
                    if (take) {
                        FailRecording(RecordingFailureStatus(current.ErrorValue()));
                        return;
                    }
                    Stop();
                    status = EditorMicrophoneTestStatus::Failed;
                    return;
                }
            }
            const auto activity = backend->CaptureActivity();
            peakLevel = activity.peakLevel;
            if (take) {
                if (const auto drained = DrainTake(); drained.HasError())
                    FailRecording(RecordingFailureStatus(drained.ErrorValue()));
                else if (activity.state == Audio::Backend::Sdl3CaptureActivityState::Capturing)
                    recordingStatus = EditorMicrophoneRecordingStatus::Recording;
                return;
            }
            status = activity.state == Audio::Backend::Sdl3CaptureActivityState::Capturing ? EditorMicrophoneTestStatus::Capturing
                                                                                           : EditorMicrophoneTestStatus::Starting;
        }

        /** @brief Drains a single queued owner operation and reconciles ongoing callback evidence. */
        void Process(const Clock::time_point now) {
            using enum EditorMicrophoneTestStatus;
            if (std::exchange(scanning, false)) {
                labels.clear();
                inputs.clear();
                status = Discover() ? Idle : Failed;
            }
            if (const auto selected = std::exchange(pending, std::nullopt)) {
                const bool recording = RecordingRequested();
                if (const auto opened = Start(*selected, now); opened.HasError()) {
                    if (recording)
                        FailRecording(RecordingFailureStatus(opened.ErrorValue()));
                    else
                        status = Failed;
                }
            }
            if (grant)
                Observe(now);
        }

        /** @brief Stops pending capture upon focus loss and expires takes before attempting to save. */
        void Advance(const bool foreground, const Clock::time_point now) {
            if (!foreground || stopping) {
                if (RecordingRequested())
                    recordingStatus = EditorMicrophoneRecordingStatus::Idle;
                Stop();
                return;
            }
            if (grant && now >= deadline) {
                if (RecordingRequested())
                    FailRecording();
                else
                    Stop();
                return;
            }
            if (saving && take)
                FinishTake();
            else
                Process(now);
        }
    };

    EditorMicrophonePermission::EditorMicrophonePermission(ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                           Assets::AssetRegistry &registry)
        : check_(std::make_unique<CaptureCheck>(mutations, files, registry)) {}

    EditorMicrophonePermission::~EditorMicrophonePermission() {
        CancelSpeechCapture(check_->generation);
    }

    /** @copydoc EditorMicrophonePermission::RefreshSpeechInputs */
    bool EditorMicrophonePermission::RefreshSpeechInputs(const bool foreground) {
        if (!foreground || check_->InputBusy() || Status() != EditorMicrophonePermissionStatus::Granted)
            return false;
        check_->labels.clear();
        check_->inputs.clear();
        try {
            return check_->Discover();
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    /** @copydoc EditorMicrophonePermission::SpeechInputAt */
    std::optional<Audio::AudioCaptureDeviceId> EditorMicrophonePermission::SpeechInputAt(const std::size_t index) const noexcept {
        if (index >= check_->inputs.size())
            return std::nullopt;
        return check_->inputs[index];
    }

    /** @copydoc EditorMicrophonePermission::BeginSpeechCapture */
    Result<EditorSpeechCaptureLease> EditorMicrophonePermission::BeginSpeechCapture(const Audio::AudioCaptureDeviceId input,
                                                                                    const bool foreground,
                                                                                    const ImmersiveAuthoringContext &owner,
                                                                                    const ImmersivePlanningContext &context) {
        using Output = Result<EditorSpeechCaptureLease>;
        if (!SpeechCaptureAllowed(foreground) || owner.Admit(context, ImmersiveRequestKind::SpeechCapture) != ImmersiveAdmission::Admitted)
            return Output::Failure(MakeError(Audio::AudioErrors::CapturePermissionDenied));
        if (check_->InputBusy())
            return Output::Failure(MakeError(Audio::AudioErrors::HandleCapacityExhausted));
        if (check_->project.empty() || std::ranges::find(check_->inputs, input) == check_->inputs.end())
            return Output::Failure(MakeError(Audio::AudioErrors::DeviceUnavailable));
        try {
            auto spec = check_->RefreshSelected(input);
            if (spec.HasError())
                return Output::Failure(spec.ErrorValue());
            return check_->OpenSpeech(input, std::move(spec).Value(), context);
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Audio::AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc EditorMicrophonePermission::ReadSpeechCapture */
    Result<Audio::Backend::AudioCaptureRead> EditorMicrophonePermission::ReadSpeechCapture(const std::uint64_t generation,
                                                                                           const bool foreground,
                                                                                           Audio::Backend::AudioCapturedBlock &block,
                                                                                           const std::span<Audio::AudioSample> pcm) {
        using Output = Result<Audio::Backend::AudioCaptureRead>;
        if (const auto allowed = CheckSpeechCapture(generation, foreground); allowed.HasError())
            return Output::Failure(allowed.ErrorValue());
        Audio::Backend::AudioCapturedBlock candidate{};
        const auto read = check_->backend->ReadCapture(generation, check_->grant, candidate, pcm);
        if (read.HasError()) {
            CancelSpeechCapture(generation);
            return Output::Failure(read.ErrorValue());
        }
        using enum Audio::Backend::AudioCaptureRead;
        const auto &value = read.Value();
        if (value.droppedFrames != 0 || candidate.missingSourceFramesBefore != 0) {
            if (value.status == Copied)
                Security::SecureZero(std::as_writable_bytes(pcm));
            CancelSpeechCapture(generation);
            return Output::Failure(MakeError(Audio::AudioErrors::SourceInvalid));
        }
        if (const auto allowed = ReconcileSpeechLease(generation, foreground); allowed.HasError()) {
            if (value.status == Copied)
                Security::SecureZero(std::as_writable_bytes(pcm));
            return Output::Failure(allowed.ErrorValue());
        }
        if (value.status == Copied)
            block = candidate;
        return Output::Success(value.status);
    }

    /** @copydoc EditorMicrophonePermission::SealSpeechCapture */
    Result<Audio::Backend::AudioCaptureBuffer> EditorMicrophonePermission::SealSpeechCapture(const std::uint64_t generation,
                                                                                             const bool foreground) {
        using Output = Result<Audio::Backend::AudioCaptureBuffer>;
        if (const auto allowed = CheckSpeechCapture(generation, foreground); allowed.HasError())
            return Output::Failure(allowed.ErrorValue());
        auto tail = check_->backend->FinishSpeech(generation, *check_->grant);
        if (tail.HasError()) {
            CancelSpeechCapture(generation);
            return tail;
        }
        if (const auto allowed = ReconcileSpeechLease(generation, foreground); allowed.HasError())
            return Output::Failure(allowed.ErrorValue());
        check_->grant.reset();
        return tail;
    }

    Result<void> EditorMicrophonePermission::CheckSpeechCapture(const std::uint64_t generation, const bool foreground) {
        auto current = ReconcileSpeechLease(generation, foreground);
        if (current.HasError())
            return current;
        if (!check_->grant)
            return Result<void>::Failure(MakeError(Audio::AudioErrors::HandleStale));
        return current;
    }

    /** @copydoc EditorMicrophonePermission::ReconcileSpeechLease */
    Result<void> EditorMicrophonePermission::ReconcileSpeechLease(const std::uint64_t generation, const bool foreground) {
        if (generation != 0 && generation == check_->expiredSpeechGeneration)
            return Result<void>::Failure(MakeError(EditorSpeechCaptureDurationLimit));
        if (!check_->OwnsSpeech(generation))
            return Result<void>::Failure(MakeError(Audio::AudioErrors::HandleStale));
        if (!SpeechCaptureAllowed(foreground)) {
            CancelSpeechCapture(generation);
            return Result<void>::Failure(MakeError(Audio::AudioErrors::CapturePermissionDenied));
        }
        if (!check_->speech->context.IsCurrent()) {
            CancelSpeechCapture(generation);
            return Result<void>::Failure(MakeError(Audio::AudioErrors::OperationCancelled));
        }
        if (check_->SpeechExpired(CaptureCheck::Clock::now())) {
            check_->ExpireSpeech();
            return Result<void>::Failure(MakeError(EditorSpeechCaptureDurationLimit));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorMicrophonePermission::CanPresentSpeech */
    bool EditorMicrophonePermission::CanPresentSpeech(const std::uint64_t generation) const noexcept {
        return check_->OwnsSpeech(generation) && check_->SpeechCurrent(CaptureCheck::Clock::now()) && SpeechCaptureAllowed(true);
    }

    /** @copydoc EditorMicrophonePermission::ReadSpeechClock */
    std::optional<Audio::AudioMonotonicTimestamp> EditorMicrophonePermission::ReadSpeechClock(
        const std::uint64_t generation) const noexcept {
        if (!check_->OwnsSpeech(generation) || !check_->grant || !check_->SpeechCurrent(CaptureCheck::Clock::now()))
            return std::nullopt;
        return Audio::AudioMonotonicTimestamp{CaptureCheck::ClockDomain, SDL_GetTicksNS()};
    }

    /** @copydoc EditorMicrophonePermission::CancelSpeechCapture */
    void EditorMicrophonePermission::CancelSpeechCapture(const std::uint64_t generation) noexcept {
        if (check_->OwnsSpeech(generation))
            check_->Stop();
    }

    /** @copydoc EditorMicrophonePermission::ScanInputs */
    void EditorMicrophonePermission::ScanInputs() noexcept {
        if (!check_->InputBusy())
            check_->scanning = true;
    }

    /** @copydoc EditorMicrophonePermission::Inputs */
    std::span<const std::string> EditorMicrophonePermission::Inputs() const noexcept {
        return check_->labels;
    }

    /** @copydoc EditorMicrophonePermission::StartTest */
    void EditorMicrophonePermission::StartTest(const std::size_t input) noexcept {
        if (check_->InputBusy() || input >= check_->inputs.size())
            return;
        check_->pending = check_->inputs[input];
        check_->status = EditorMicrophoneTestStatus::Starting;
    }

    /** @copydoc EditorMicrophonePermission::StopTest */
    void EditorMicrophonePermission::StopTest() noexcept {
        if (check_->RecordingRequested() || check_->speech)
            return;
        if (check_->grant)
            check_->stopping = true;
        else
            check_->Stop();
    }

    /** @copydoc EditorMicrophonePermission::AdvanceTest */
    void EditorMicrophonePermission::AdvanceTest(const bool foreground) noexcept {
        check_->AdvanceMonitor(foreground);
        try {
            check_->PollPublication();
            if (check_->speech) {
                // This tick's foreground flag belongs to Audio Settings. Play owns its own focus fence.
                static_cast<void>(ReconcileSpeechLease(check_->generation, true));
            } else if (check_->scanning || check_->pending || check_->grant) {
                check_->Advance(foreground, CaptureCheck::Clock::now());
            }
        } catch (const std::bad_alloc &) {
            if (check_->RecordingRequested())
                check_->FailRecording();
            else {
                check_->Stop();
                check_->status = EditorMicrophoneTestStatus::Failed;
            }
        }
    }

    /** @copydoc EditorMicrophonePermission::TestStatus */
    EditorMicrophoneTestStatus EditorMicrophonePermission::TestStatus() const noexcept {
        return check_->status;
    }

    /** @copydoc IEditorMicrophonePermission::TestPeakLevel */
    float EditorMicrophonePermission::TestPeakLevel() const noexcept {
        return check_->status == EditorMicrophoneTestStatus::Capturing ? check_->peakLevel : 0.0F;
    }

    /** @copydoc IEditorMicrophoneRecorder::SetProject */
    void EditorMicrophonePermission::SetProject(const std::filesystem::path &root) {
        if (root == check_->project)
            return;
        CancelSpeechCapture(check_->generation);
        check_->monitor.Advance(false, false);
        CancelRecording();
        check_->publication.Detach();
        check_->recordingStatus = EditorMicrophoneRecordingStatus::Idle;
        check_->savedRecording.path.clear();
        check_->project = root;
    }

    /** @copydoc IEditorMicrophoneRecorder::CanRecord */
    bool EditorMicrophonePermission::CanRecord() const noexcept {
        return !check_->project.empty() && !check_->InputBusy() && !check_->publication.Busy();
    }

    /** @copydoc IEditorMicrophoneRecorder::StartRecording */
    void EditorMicrophonePermission::StartRecording(const std::size_t input, const std::string_view wavePath) {
        if (!CanRecord() || input >= check_->inputs.size())
            return;
        check_->savedRecording.path.clear();
        check_->savedRecording.asset.reset();
        try {
            auto target = RecordingTarget(check_->project, wavePath);
            if (target) {
                check_->recordingTarget = std::move(target);
                check_->pending = check_->inputs[input];
                check_->status = EditorMicrophoneTestStatus::Idle;
                check_->recordingStatus = EditorMicrophoneRecordingStatus::Starting;
                return;
            }
        } catch (const std::bad_alloc &) {
            check_->recordingTarget.reset();
        }
        check_->recordingStatus = EditorMicrophoneRecordingStatus::Failed;
    }

    /** @copydoc IEditorMicrophoneRecorder::SaveRecording */
    void EditorMicrophonePermission::SaveRecording() noexcept {
        if (check_->take)
            check_->saving = true;
    }

    /** @copydoc IEditorMicrophoneRecorder::CancelRecording */
    void EditorMicrophonePermission::CancelRecording() noexcept {
        if (check_->RecordingRequested())
            check_->stopping = true;
        check_->publication.Cancel();
        check_->savedRecording.asset.reset();
        if (check_->recordingStatus != EditorMicrophoneRecordingStatus::Saving)
            check_->recordingStatus = EditorMicrophoneRecordingStatus::Idle;
    }

    /** @copydoc IEditorMicrophoneRecorder::RecordingStatus */
    EditorMicrophoneRecordingStatus EditorMicrophonePermission::RecordingStatus() const noexcept {
        return check_->recordingStatus;
    }

    /** @copydoc IEditorMicrophoneRecorder::RecordingPeakLevel */
    float EditorMicrophonePermission::RecordingPeakLevel() const noexcept {
        return check_->take ? check_->peakLevel : 0.0F;
    }

    /** @copydoc IEditorMicrophoneRecorder::SavedRecordingPath */
    std::string_view EditorMicrophonePermission::SavedRecordingPath() const noexcept {
        return check_->savedRecording.path;
    }

    /** @copydoc IEditorMicrophoneRecorder::TakeSavedRecording */
    std::optional<Assets::AssetId> EditorMicrophonePermission::TakeSavedRecording() noexcept {
        return std::exchange(check_->savedRecording.asset, std::nullopt);
    }

    /** @copydoc EditorMicrophonePermission::MonitorControls */
    IEditorMicrophoneMonitor &EditorMicrophonePermission::MonitorControls() noexcept {
        return check_->monitor;
    }
}  // namespace Horo::Editor
