#include "EditorAudioPreviewHost.h"

#include "EditorAudioPreviewPreparation.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"
#if HORO_EDITOR_AUDIO_SDL3
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"
#endif
#include <array>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Editor {
    namespace {
        /** @brief Uses the one host monotonic domain; wall-clock time never enters Audio deadlines. */
        Audio::AudioMonotonicTimestamp Deadline() {
            return {1,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) +
                        2'000'000'000ULL};
        }

        /** @brief Control-owner adapter tracking exact operation admission and acknowledgement.
         * @details Idle output never calls peer AdvanceControl. Headless callback progression is
         * independent of control work, so steady Rendering continues without inventing an operation.
         * The selected peer and both borrowed driver closures share this one retained owner.
         */
        class DrivenOutput final : public Audio::Backend::AudioBackend {
        public:
            explicit DrivenOutput(EditorAudioPreviewBackend selected) : selected_(std::move(selected)) {}

            Audio::AudioBackendKind Kind() const noexcept override {
                return selected_.output->Kind();
            }

            Audio::AudioRuntimeId Owner() const noexcept override {
                return selected_.output->Owner();
            }

            Result<Audio::Backend::OperationId> Begin(const Audio::Backend::Request &request,
                                                      const Audio::AudioMonotonicTimestamp &deadline) override {
                auto result = selected_.output->Begin(request, deadline);
                if (result.HasValue())
                    operation_ = result.Value();
                return result;
            }

            Result<void> CommitRendering(const Audio::AudioDeviceEpoch &epoch) override {
                return selected_.output->CommitRendering(epoch);
            }

            Result<Audio::Backend::CancelDisposition> Cancel(const Audio::Backend::OperationId &operation) override {
                return selected_.output->Cancel(operation);
            }

            Result<std::optional<Audio::Backend::Completion>> Poll(const Audio::Backend::OperationId &operation) override {
                return selected_.output->Poll(operation);
            }

            Result<void> AcknowledgeCompletion(const Audio::Backend::OperationId &operation) override {
                auto result = selected_.output->AcknowledgeCompletion(operation);
                if (result.HasValue())
                    operation_.reset();
                return result;
            }

            std::size_t DrainEvents(std::span<Audio::Backend::Event> events) noexcept override {
                return selected_.output->DrainEvents(events);
            }

            Audio::Backend::AudioCallbackViolationDrain DrainSafetyViolations(
                std::span<Audio::Backend::AudioCallbackViolation> violations) noexcept override {
                return selected_.output->DrainSafetyViolations(violations);
            }

            Result<void> Advance() {
                if (operation_) {
                    const auto completion = selected_.output->Poll(*operation_);
                    if (completion.HasError())
                        return Result<void>::Failure(completion.ErrorValue());
                    if (!completion.Value()) {
                        auto advanced = selected_.advanceControl();
                        if (advanced.HasError())
                            return advanced;
                    }
                }
                return selected_.advanceCallback ? selected_.advanceCallback() : Result<void>::Success();
            }

        private:
            EditorAudioPreviewBackend selected_;
            std::optional<Audio::Backend::OperationId> operation_;
        };

        /** @brief Reconciles an accepted closed-peer discovery operation before its backend can be destroyed.
         * @details Enumerate has no Open/render port or lifecycle events. AudioBackend requires every
         * operation acknowledged even when the original query fails. Unproven/Retained cancellation
         * follows ADR-062 process-fatal retention; terminating here does not unwind the owned peer.
         */
        class DiscoveryOperation final {
        public:
            DiscoveryOperation(Audio::Backend::AudioBackend &backend, Audio::Backend::OperationId operation)
                : backend_(backend), operation_(operation) {}

            DiscoveryOperation(const DiscoveryOperation &) = delete;
            DiscoveryOperation &operator=(const DiscoveryOperation &) = delete;

            ~DiscoveryOperation() {
                try {
                    if (acknowledged_)
                        return;
                    if (backend_.Cancel(operation_).HasError())
                        std::terminate();
                    auto terminal = backend_.Poll(operation_);
                    if (terminal.HasError() || !terminal.Value())
                        std::terminate();
                    if (Acknowledge(*terminal.Value()).HasError())
                        std::terminate();
                } catch (...) {
                    // The accepted native operation must not escape cleanup by unwinding its backend owner.
                    std::terminate();
                }
            }

            Result<void> Acknowledge(const Audio::Backend::Completion &completion) {
                using Audio::Backend::ResourceDisposition;
                if (const auto *failure = std::get_if<Audio::Backend::Failed>(&completion.outcome);
                    failure && failure->resources == ResourceDisposition::Retained)
                    std::terminate();
                if (const auto *cancelled = std::get_if<Audio::Backend::Cancelled>(&completion.outcome);
                    cancelled && cancelled->resources == ResourceDisposition::Retained)
                    std::terminate();
                auto result = backend_.AcknowledgeCompletion(operation_);
                acknowledged_ = result.HasValue();
                return result;
            }

        private:
            Audio::Backend::AudioBackend &backend_;
            Audio::Backend::OperationId operation_;
            bool acknowledged_{};
        };

        /** @brief Captures one closed-peer catalog through the actual bounded native protocol. */
        Result<Audio::AudioDeviceSnapshot> Discover(DrivenOutput &backend) {
            auto operation = backend.Begin(Audio::Backend::Enumerate{}, Deadline());
            if (operation.HasError())
                return Result<Audio::AudioDeviceSnapshot>::Failure(operation.ErrorValue());
            DiscoveryOperation pending{backend, operation.Value()};
            if (auto advanced = backend.Advance(); advanced.HasError())
                return Result<Audio::AudioDeviceSnapshot>::Failure(advanced.ErrorValue());
            auto completion = backend.Poll(operation.Value());
            if (completion.HasError())
                return Result<Audio::AudioDeviceSnapshot>::Failure(completion.ErrorValue());
            if (!completion.Value())
                return Result<Audio::AudioDeviceSnapshot>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
            if (auto acknowledged = pending.Acknowledge(*completion.Value()); acknowledged.HasError())
                return Result<Audio::AudioDeviceSnapshot>::Failure(acknowledged.ErrorValue());
            const auto &outcome = completion.Value()->outcome;
            if (const auto *catalog = std::get_if<Audio::AudioDeviceSnapshot>(&outcome))
                return Result<Audio::AudioDeviceSnapshot>::Success(*catalog);
            if (const auto *failure = std::get_if<Audio::Backend::Failed>(&outcome))
                return Result<Audio::AudioDeviceSnapshot>::Failure(failure->error);
            return Result<Audio::AudioDeviceSnapshot>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        }

        /** @brief Detached prepared output and its control driver, adopted together only after composition succeeds. */
        struct PreparedLane final {
            std::unique_ptr<Audio::AudioFrontend> frontend;
            std::function<Result<void>()> advance;
        };

        /** @brief Performs bounded file/device preparation on control, before any native output opens. */
        template <typename Factory>
        Result<PreparedLane> PrepareLane(const AudioComponentPreviewRequest &request, Audio::AudioRuntimeId owner, const Factory &factory) {
            auto decoded = AudioPreviewHostDetail::Decode(request);
            if (decoded.HasError())
                return Result<PreparedLane>::Failure(decoded.ErrorValue());
            auto selectedBackend = factory(owner);
            if (selectedBackend.HasError())
                return Result<PreparedLane>::Failure(selectedBackend.ErrorValue());
            auto selectedOutput = std::move(selectedBackend).Value();
            if (!selectedOutput.output || !selectedOutput.advanceControl)
                return Result<PreparedLane>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
            auto backend = std::make_unique<DrivenOutput>(std::move(selectedOutput));
            auto catalog = Discover(*backend);
            if (catalog.HasError())
                return Result<PreparedLane>::Failure(catalog.ErrorValue());
            const auto selected = Audio::ResolveAudioDevice(catalog.Value(), Audio::AudioDefaultDeviceRole::Multimedia);
            if (selected.status != Audio::AudioDeviceResolutionStatus::Resolved)
                return Result<PreparedLane>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
            auto preparation = AudioPreviewHostDetail::Prepare(request, decoded.Value(), owner);
            if (preparation.HasError())
                return Result<PreparedLane>::Failure(preparation.ErrorValue());
            auto prepared = std::move(preparation).Value();
            const Audio::AudioDeviceEpoch epoch{selected.device, 1, 1};
            auto *driver = backend.get();
            const auto advance = [driver] {
                return driver->Advance();
            };
            Audio::Internal::AudioFrontendOutput
                output{std::move(backend),
                       {epoch,
                        {selected.device, {48'000, Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Stereo)}, {}, {256, 256, 256}}},
                       std::move(catalog).Value(),
                       1};
            auto frontend = Audio::Internal::AudioFrontendComposition::Create(prepared, output);
            if (frontend.HasError()) {
                AudioPreviewHostDetail::RetireDetached(prepared);
                return Result<PreparedLane>::Failure(frontend.ErrorValue());
            }
            return Result<PreparedLane>::Success({std::move(frontend).Value(), advance});
        }
    }  // namespace

    /** @copydoc MakeSdl3AudioPreviewOutput */
    Result<EditorAudioPreviewBackend> MakeSdl3AudioPreviewOutput(const Audio::AudioRuntimeId owner) {
#if HORO_EDITOR_AUDIO_SDL3
        auto created = Audio::Backend::CreateSdl3AudioBackend({owner, 1});
        if (created.HasError())
            return Result<EditorAudioPreviewBackend>::Failure(created.ErrorValue());
        auto *backend = created.Value().get();
        return Result<EditorAudioPreviewBackend>::Success({std::move(created).Value(), [backend] {
            return backend->AdvanceControl();
        }});
#else
        static_cast<void>(owner);
        return Result<EditorAudioPreviewBackend>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
#endif
    }

    /** @copydoc MakeNullAudioPreviewOutput */
    Result<EditorAudioPreviewBackend> MakeNullAudioPreviewOutput(const Audio::AudioRuntimeId owner) {
        auto created = Audio::Backend::CreateNullAudioBackend({owner, 1, 1, 1});
        if (created.HasError())
            return Result<EditorAudioPreviewBackend>::Failure(created.ErrorValue());
        auto *backend = created.Value().get();
        return Result<EditorAudioPreviewBackend>::Success({std::move(created).Value(), [backend] {
            return backend->AdvanceControl();
        }, [backend] {
            using enum Audio::Backend::NullAudioBackendState;
            if (backend->State() == Priming || backend->State() == Rendering || backend->State() == Quiescing) {
                const auto rendered = backend->AdvanceCallback();
                if (rendered.HasError())
                    return Result<void>::Failure(rendered.ErrorValue());
            }
            return Result<void>::Success();
        }});
    }

    struct EditorAudioPreviewHost::State final {
        std::uint64_t nextRuntime{1};  // Host-owned, never reused across workspace/document switches.
        AudioComponentPreviewSnapshot snapshot;
        std::unique_ptr<Audio::AudioFrontend> frontend;
        EditorAudioPreviewOutputFactory factory;
        std::function<Result<void>()> advance;  // Borrows only the backend retained by frontend; cleared after Closed.
        bool startVoice{};
    };

    EditorAudioPreviewHost::EditorAudioPreviewHost(EditorAudioPreviewOutputFactory outputFactory) : state_(std::make_unique<State>()) {
        state_->factory = std::move(outputFactory);
    }

    EditorAudioPreviewHost::~EditorAudioPreviewHost() {
        try {
            if (!state_->frontend)
                return;
            state_->frontend->Close();
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (state_->frontend && std::chrono::steady_clock::now() < end) {
                static_cast<void>(Pump());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (state_->frontend)
                std::terminate();  // A timeout never proves native callback detachment; retain/fatal Audio ownership contract.
        } catch (...) {
            // Any cleanup exception leaves native detachment unproven; do not unwind and free the retained output.
            std::terminate();
        }
    }

    /** @copydoc IAudioComponentPreview::Start */
    Result<void> EditorAudioPreviewHost::Start(const AudioComponentPreviewRequest &request) {
        if (!request.workspace.IsValid() || request.revision == 0 || request.object == 0)
            return Result<void>::Failure(MakeError(AudioComponentPreviewErrors::Stale));
        if (state_->frontend)
            return Result<void>::Failure(MakeError(AudioComponentPreviewErrors::Busy));
        state_->snapshot = {.workspace = request.workspace, .revision = request.revision, .object = request.object};
        const auto fail = [this](const Error &error) {
            state_->snapshot.failure = error;
            return Result<void>::Failure(error);
        };
        if (request.source.playback.spatialMode == Audio::AudioSpatialMode::ThreeD && !request.listener)
            return fail(MakeError(AudioComponentPreviewErrors::MissingListener));
        if (auto valid = Audio::ValidateAudioSoundReference(request.source.sound); valid.HasError())
            return fail(valid.ErrorValue());
        if (auto valid = Audio::ValidateAudioSoundPlaybackDefaults(request.source.playback); valid.HasError())
            return fail(valid.ErrorValue());
        if (!state_->factory)
            return fail(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        if (state_->nextRuntime == std::numeric_limits<std::uint64_t>::max())
            return fail(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        const auto owner = Audio::AudioRuntimeId::Create(state_->nextRuntime++).Value();
        auto prepared = PrepareLane(request, owner, state_->factory);
        if (prepared.HasError())
            return fail(prepared.ErrorValue());
        auto lane = std::move(prepared).Value();
        state_->frontend = std::move(lane.frontend);
        state_->advance = std::move(lane.advance);
        state_->startVoice = true;
        if (auto started = state_->frontend->Start(Deadline()); started.HasError()) {
            state_->frontend->Close();
            return fail(started.ErrorValue());
        }
        state_->snapshot.output = state_->frontend->Snapshot();
        return Result<void>::Success();
    }

    /** @copydoc IAudioComponentPreview::Stop */
    void EditorAudioPreviewHost::Stop(const Assets::AssetId workspace) noexcept {
        if (state_->frontend && state_->snapshot.workspace == workspace) {
            state_->startVoice = false;
            state_->frontend->Close();
        }
    }

    /** @copydoc IAudioComponentPreview::Pump */
    Result<void> EditorAudioPreviewHost::Pump() {
        if (!state_->frontend)
            return Result<void>::Success();
        const auto recordFailure = [this](const Error &error) {
            if (!state_->snapshot.failure)
                state_->snapshot.failure = error;
        };
        const auto advanced = state_->advance();
        if (advanced.HasError()) {
            recordFailure(advanced.ErrorValue());
            state_->frontend->Close();
        }
        auto pumped = state_->frontend->Pump(Deadline());
        if (pumped.HasError())
            recordFailure(pumped.ErrorValue());
        state_->snapshot.output = state_->frontend->Snapshot();
        if (state_->startVoice && state_->snapshot.output->phase == Audio::AudioFrontendPhase::Active) {
            auto admitted = state_->frontend->Transport({state_->snapshot.output->voice, Audio::AudioVoiceControl::Start});
            if (admitted.HasError()) {
                recordFailure(admitted.ErrorValue());
                state_->frontend->Close();
            } else if (admitted.Value().status == Audio::AudioCommandStagingStatus::Ok)
                state_->startVoice = false;
        }
        std::array<Audio::AudioFrontendOperationResult, Audio::MaximumAudioFrontendOperations> results;
        const auto count = state_->frontend->DrainTransportResults(results);
        for (std::size_t i = 0; i < count; ++i)
            if (results[i].error)
                recordFailure(MakeError(*results[i].error));
        if (state_->snapshot.output->phase == Audio::AudioFrontendPhase::Closed) {
            state_->frontend.reset();
            state_->advance = {};
        }
        return advanced.HasError() ? Result<void>::Failure(advanced.ErrorValue()) : std::move(pumped);
    }

    /** @copydoc IAudioComponentPreview::Snapshot */
    AudioComponentPreviewSnapshot EditorAudioPreviewHost::Snapshot() const {
        auto snapshot = state_->snapshot;
        if (state_->frontend)
            snapshot.output = state_->frontend->Snapshot();
        return snapshot;
    }
}  // namespace Horo::Editor
