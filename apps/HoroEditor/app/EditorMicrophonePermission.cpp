#include "EditorMicrophonePermission.h"

#include "EditorMicrophoneRecording.h"
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"
#include "Horo/Platform/SecureRandom.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        /** @brief Rejects characters that cannot be used consistently in project asset paths. */
        bool InvalidRecordingPathCharacter(const unsigned char character) noexcept {
            constexpr std::string_view Reserved = ":*?\"<>|";
            return character < 32 || character == 127 || Reserved.find(static_cast<char>(character)) != std::string_view::npos;
        }

        /** @brief Projects exact OS facts without interpreting permission as a live microphone session. */
        EditorMicrophonePermissionStatus Project(const Audio::Backend::Sdl3MacCapturePermission native) noexcept {
            using enum EditorMicrophonePermissionStatus;
            if (native.pending)
                return Pending;
            switch (native.permission) {
                case Audio::AudioCapturePermission::Required:
                    return Required;
                case Audio::AudioCapturePermission::Granted:
                    return Granted;
                case Audio::AudioCapturePermission::Denied:
                    return Denied;
                default:
                    return Unavailable;
            }
        }

        /** @brief Restricts recording to portable WAV destinations under the project asset root. */
        bool IsRecordingPath(const std::string_view path) noexcept {
            return path.size() <= 200 && path.starts_with("assets/") && path.ends_with(".wav") && !path.ends_with("/.wav") &&
                   !std::ranges::any_of(path, InvalidRecordingPathCharacter);
        }

        /** @brief Reserves the exact new recording identity for a canonical asset path. */
        std::optional<Application::Internal::AudioRecordingDestination> RecordingTarget(const std::filesystem::path &root,
                                                                                        const std::string_view path) {
            if (root.empty() || !IsRecordingPath(path))
                return std::nullopt;
            auto relative = ProjectPath::Parse(path);
            if (relative.HasError() || relative.Value().String() != path)
                return std::nullopt;
            std::array<std::uint8_t, 16> bytes{};
            if (const auto entropy = Platform::CreateNativeSecureRandomSource();
                entropy->Fill(std::as_writable_bytes(std::span{bytes})).HasError())
                return std::nullopt;
            const auto id = Assets::AssetId::FromBytes(bytes);
            if (!id.IsValid())
                return std::nullopt;
            return Application::Internal::AudioRecordingDestination{id.ToString(), root, std::move(relative).Value(), id};
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
        std::uint64_t generation{};
        Clock::time_point deadline{};
        Clock::time_point nextHealth{};
        EditorMicrophoneTestStatus status{EditorMicrophoneTestStatus::Idle};
        float peakLevel{};
        std::filesystem::path project;
        std::optional<Application::Internal::AudioRecordingDestination> recordingTarget;
        std::optional<EditorRecordingTake> take;
        EditorRecordingPublication publication;
        EditorMicrophoneRecordingStatus recordingStatus{EditorMicrophoneRecordingStatus::Idle};
        std::string savedPath;

        CaptureCheck(ProjectMutationCoordinator &mutations, DurableFileSystem &files, Assets::AssetRegistry &registry)
            : publication(mutations, files, registry) {}

        /** @brief Returns pending or active private PCM ownership, separate from detached publication. */
        [[nodiscard]] bool RecordingRequested() const noexcept {
            return recordingTarget.has_value() || take.has_value();
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

        /** @brief Refreshes the exact selected endpoint, then opens a new foreground-only generation. */
        bool Start(const Audio::AudioCaptureDeviceId selected, const Clock::time_point now) {
            if (!backend || generation == std::numeric_limits<std::uint64_t>::max())
                return false;
            const auto discovery = backend->DiscoverCapture();
            if (discovery.HasError())
                return false;
            const auto &snapshot = discovery.Value().snapshot;
            const auto device = std::ranges::find(snapshot.devices, selected, &Audio::AudioDiscoveredCaptureDevice::id);
            if (device == snapshot.devices.end() || !device->preferredFormat)
                return false;
            return Open(selected, snapshot.revision, *device->preferredFormat, now);
        }

        /** @brief Prepares private recording storage before opening the exact admitted native stream. */
        bool Open(const Audio::AudioCaptureDeviceId selected, const std::uint64_t revision, const Audio::AudioProcessingFormat &format,
                  const Clock::time_point now) {
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
                    return false;
                take.emplace(std::move(prepared).Value());
            }
            if (backend->StartCapture(request, intent, selectedGrant).HasError())
                return false;
            grant = selectedGrant;
            deadline = now + (recording ? std::chrono::seconds(20) : TestDuration);
            nextHealth = now;
            return true;
        }

        /** @brief Cancels queued operations and detaches the current session before presentation clears. */
        void Stop() noexcept {
            scanning = false;
            stopping = false;
            saving = false;
            pending.reset();
            if (grant)
                static_cast<void>(backend->StopCapture(grant->generation));
            grant.reset();
            status = EditorMicrophoneTestStatus::Idle;
            peakLevel = 0.0F;
            recordingTarget.reset();
            take.reset();
        }

        /** @brief Retires a failed take without writing any partial asset. */
        void FailRecording() noexcept {
            Stop();
            recordingStatus = EditorMicrophoneRecordingStatus::Failed;
        }

        /** @brief Drains a bounded number of complete microphone blocks per editor update. */
        bool DrainTake() {
            using enum Audio::Backend::AudioCaptureRead;
            for (unsigned i = 0; i < 4; ++i) {
                Audio::Backend::AudioCapturedBlock block;
                const auto read = backend->ReadCapture(grant->generation, block, take->scratch);
                if (read.HasError() || read.Value().droppedFrames != 0)
                    return false;
                if (read.Value().status == Empty)
                    return true;
                if (read.Value().status != Copied || take->Append(block).HasError())
                    return false;
            }
            return true;
        }

        /** @brief Detaches an explicitly saved take and transfers publication off the editor lane. */
        void FinishTake() {
            if (auto tail = backend->FinishRecording(grant->generation, *grant);
                tail.HasError() || !publication.Start(std::move(*take), std::move(tail).Value())) {
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
                recordingStatus = Failed;
                return;
            }
            savedPath = std::move(*result).Value();
            recordingStatus = Saved;
        }

        /** @brief Reconciles native health periodically and publishes callback activity without reading audio. */
        void Observe(const Clock::time_point now) {
            if (now >= nextHealth) {
                nextHealth = now + HealthInterval;
                if (backend->ReconcileCapture(grant->generation, grant).HasError()) {
                    if (take) {
                        FailRecording();
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
                if (!DrainTake())
                    FailRecording();
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
            const bool recording = RecordingRequested();
            if (const auto selected = std::exchange(pending, std::nullopt); selected && !Start(*selected, now)) {
                if (recording)
                    FailRecording();
                else
                    status = Failed;
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

    EditorMicrophonePermission::~EditorMicrophonePermission() = default;

    /** @copydoc EditorMicrophonePermission::Status */
    EditorMicrophonePermissionStatus EditorMicrophonePermission::Status() {
        if (const auto now = std::chrono::steady_clock::now(); now >= nextObservation_) {
            status_ = Project(Audio::Backend::ObserveSdl3MacCapturePermission());
            nextObservation_ = now + std::chrono::milliseconds(250);
        }
        return status_;
    }

    /** @copydoc EditorMicrophonePermission::Request */
    void EditorMicrophonePermission::Request() {
        if (Status() != EditorMicrophonePermissionStatus::Required)
            return;
        const auto requested = Audio::Backend::RequestSdl3MacCapturePermission();
        nextObservation_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        status_ = requested.HasValue() ? Project(Audio::Backend::ObserveSdl3MacCapturePermission())
                                       : EditorMicrophonePermissionStatus::Unavailable;
    }

    /** @copydoc EditorMicrophonePermission::ScanInputs */
    void EditorMicrophonePermission::ScanInputs() noexcept {
        if (!check_->grant && !check_->pending)
            check_->scanning = true;
    }

    /** @copydoc EditorMicrophonePermission::Inputs */
    std::span<const std::string> EditorMicrophonePermission::Inputs() const noexcept {
        return check_->labels;
    }

    /** @copydoc EditorMicrophonePermission::StartTest */
    void EditorMicrophonePermission::StartTest(const std::size_t input) noexcept {
        if (check_->grant || check_->pending || check_->scanning || input >= check_->inputs.size())
            return;
        check_->pending = check_->inputs[input];
        check_->status = EditorMicrophoneTestStatus::Starting;
    }

    /** @copydoc EditorMicrophonePermission::StopTest */
    void EditorMicrophonePermission::StopTest() noexcept {
        if (check_->RecordingRequested())
            return;
        if (check_->grant)
            check_->stopping = true;
        else
            check_->Stop();
    }

    /** @copydoc EditorMicrophonePermission::AdvanceTest */
    void EditorMicrophonePermission::AdvanceTest(const bool foreground) noexcept {
        try {
            check_->PollPublication();
            if (check_->scanning || check_->pending || check_->grant)
                check_->Advance(foreground, CaptureCheck::Clock::now());
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
        CancelRecording();
        check_->savedPath.clear();
        check_->project = root;
    }

    /** @copydoc IEditorMicrophoneRecorder::CanRecord */
    bool EditorMicrophonePermission::CanRecord() const noexcept {
        return !check_->project.empty() && !check_->grant && !check_->pending && !check_->scanning && !check_->publication.Busy();
    }

    /** @copydoc IEditorMicrophoneRecorder::StartRecording */
    void EditorMicrophonePermission::StartRecording(const std::size_t input, const std::string_view wavePath) {
        if (!CanRecord() || input >= check_->inputs.size())
            return;
        check_->savedPath.clear();
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
        return check_->savedPath;
    }
}  // namespace Horo::Editor
