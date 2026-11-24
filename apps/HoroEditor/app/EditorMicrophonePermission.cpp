#include "EditorMicrophonePermission.h"

#include "Horo/Audio/Internal/Sdl3AudioBackend.h"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
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
    }  // namespace

    /** @brief Owns an ephemeral foreground connection check and never exposes PCM to editor services. */
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
        std::optional<Audio::Backend::Sdl3CaptureGrant> grant;
        std::uint64_t generation{};
        Clock::time_point deadline{};
        Clock::time_point nextHealth{};
        EditorMicrophoneTestStatus status{EditorMicrophoneTestStatus::Idle};

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
            ++generation;
            const Audio::Backend::AudioCaptureBufferDescriptor request{.device = selected,
                                                                       .generation = generation,
                                                                       .clockDomain = ClockDomain,
                                                                       .format = *device->preferredFormat,
                                                                       .slots = 2,
                                                                       .maximumFrames = 4096,
                                                                       .budgetBytes = 128 * 1024};
            const Audio::Backend::Sdl3CaptureGrant selectedGrant{.device = selected,
                                                                 .discoveryRevision = snapshot.revision,
                                                                 .generation = generation,
                                                                 .authorityRevision = generation,
                                                                 .intent = Intent,
                                                                 .foreground = true};
            if (backend->StartCapture(request, Intent, selectedGrant).HasError())
                return false;
            grant = selectedGrant;
            deadline = now + TestDuration;
            nextHealth = now;
            return true;
        }

        /** @brief Cancels queued operations and detaches the current session before presentation clears. */
        void Stop() noexcept {
            scanning = false;
            stopping = false;
            pending.reset();
            if (grant)
                static_cast<void>(backend->StopCapture(grant->generation));
            grant.reset();
            status = EditorMicrophoneTestStatus::Idle;
        }

        /** @brief Reconciles native health periodically and publishes callback activity without reading audio. */
        void Observe(const Clock::time_point now) {
            if (now >= nextHealth) {
                nextHealth = now + HealthInterval;
                if (backend->ReconcileCapture(grant->generation, grant).HasError()) {
                    Stop();
                    status = EditorMicrophoneTestStatus::Failed;
                    return;
                }
            }
            status = backend->CaptureActivity().state == Audio::Backend::Sdl3CaptureActivityState::Capturing
                         ? EditorMicrophoneTestStatus::Capturing
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
            if (const auto selected = std::exchange(pending, std::nullopt); selected && !Start(*selected, now))
                status = Failed;
            if (grant)
                Observe(now);
        }
    };

    EditorMicrophonePermission::EditorMicrophonePermission() : check_(std::make_unique<CaptureCheck>()) {}

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
        if (check_->grant)
            check_->stopping = true;
        else
            check_->Stop();
    }

    /** @copydoc EditorMicrophonePermission::AdvanceTest */
    void EditorMicrophonePermission::AdvanceTest(const bool foreground) noexcept {
        if (!check_->scanning && !check_->pending && !check_->grant)
            return;
        const auto now = CaptureCheck::Clock::now();
        if (!foreground || check_->stopping || (check_->grant && now >= check_->deadline)) {
            check_->Stop();
            return;
        }
        try {
            check_->Process(now);
        } catch (const std::bad_alloc &) {
            check_->Stop();
            check_->status = EditorMicrophoneTestStatus::Failed;
        }
    }

    /** @copydoc EditorMicrophonePermission::TestStatus */
    EditorMicrophoneTestStatus EditorMicrophonePermission::TestStatus() const noexcept {
        return check_->status;
    }
}  // namespace Horo::Editor
