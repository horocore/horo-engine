#include "EditorAudioPreviewHost.h"

#include "EditorAudioPreviewPreparation.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"
#if HORO_EDITOR_AUDIO_SDL3
#include "Horo/Audio/Internal/Sdl3AudioBackend.h"
#endif
#if defined(HORO_EDITOR_AUDIO_WASAPI) && HORO_EDITOR_AUDIO_WASAPI
#include "Horo/Audio/Internal/WasapiAudioBackend.h"
#endif
#if defined(HORO_EDITOR_AUDIO_COREAUDIO) && HORO_EDITOR_AUDIO_COREAUDIO
#include "Horo/Audio/Internal/CoreAudioBackend.h"
#endif
#if defined(HORO_EDITOR_AUDIO_PIPEWIRE) && HORO_EDITOR_AUDIO_PIPEWIRE
#include "Horo/Audio/Internal/PipeWireAudioBackend.h"
#endif
#include <array>
#include <chrono>
#include <thread>

namespace Horo::Editor {
    namespace {
#if (defined(HORO_EDITOR_AUDIO_WASAPI) && HORO_EDITOR_AUDIO_WASAPI) ||                                                                     \
    (defined(HORO_EDITOR_AUDIO_COREAUDIO) && HORO_EDITOR_AUDIO_COREAUDIO) ||                                                               \
    (defined(HORO_EDITOR_AUDIO_PIPEWIRE) && HORO_EDITOR_AUDIO_PIPEWIRE)
        /** @brief Retain a concrete native peer and its explicit control driver together, before any native output acquisition. */
        Result<EditorAudioPreviewBackend> NativeOutput(Result<std::unique_ptr<Audio::Backend::NativeAudioBackend>> created) {
            if (created.HasError())
                return Result<EditorAudioPreviewBackend>::Failure(created.ErrorValue());
            auto *driver = created.Value().get();
            return Result<EditorAudioPreviewBackend>::Success({std::move(created).Value(), [driver] {
                return driver->AdvanceControl();
            }});
        }
#endif
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

            Result<void> AdvanceControl() {
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
                return Result<void>::Success();
            }

            Result<void> Advance() {
                if (auto advanced = AdvanceControl(); advanced.HasError())
                    return advanced;
                return selected_.advanceCallback ? selected_.advanceCallback() : Result<void>::Success();
            }

        private:
            EditorAudioPreviewBackend selected_;
            std::optional<Audio::Backend::OperationId> operation_;
        };

        /** @brief Reconciles an accepted pre-callback preparation operation before its backend can be destroyed.
         * @details Enumerate/Open have no attached render port. AudioBackend requires every
         * operation acknowledged even when the original query fails. The preparation owner then
         * drives explicit Close; a query completion never proves discovery resources were retired.
         */
        class PreparationOperation final {
        public:
            PreparationOperation(Audio::Backend::AudioBackend &backend, Audio::Backend::OperationId operation)
                : backend_(backend), operation_(operation) {}

            PreparationOperation(const PreparationOperation &) = delete;
            PreparationOperation &operator=(const PreparationOperation &) = delete;

            ~PreparationOperation() {
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
                if (completion.operation != operation_)
                    return Result<void>::Failure(MakeError(Audio::AudioErrors::IdentityInvalid));
                auto result = backend_.AcknowledgeCompletion(operation_);
                acknowledged_ = result.HasValue();
                return result;
            }

            /** @brief Disarm only after successful frontend composition acknowledged and adopted this exact Open. */
            void Adopted() noexcept {
                acknowledged_ = true;
            }

        private:
            Audio::Backend::AudioBackend &backend_;
            Audio::Backend::OperationId operation_;
            bool acknowledged_{};
        };

        /** @brief Owns discovery resources until successful frontend adoption or exact Close completion.
         * @details No render port was attached during preparation. Native discovery may still own a
         * thread loop, apartment or listeners, including after failed/cancelled enumeration. Cleanup
         * pumps only control with one absolute deadline. Unproven retirement terminates without
         * unwinding this owner, matching the host's existing active-lane shutdown policy.
         */
        class PreparedOutput final {
        public:
            explicit PreparedOutput(std::unique_ptr<DrivenOutput> backend) : driver_(*backend) {
                output.backend = std::move(backend);
            }

            PreparedOutput(const PreparedOutput &) = delete;
            PreparedOutput &operator=(const PreparedOutput &) = delete;

            ~PreparedOutput() {
                if (output.backend)
                    RetireDiscovery();
            }

            /** @brief Close a rejected discovery/preparation prefix before releasing its selected backend. */
            void RetireDiscovery() noexcept {
                try {
                    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    const auto deadline = Deadline();
                    const auto close = driver_.Begin(Audio::Backend::Close{}, deadline);
                    if (close.HasError())
                        std::terminate();
                    while (std::chrono::steady_clock::now() < end) {
                        auto completion = driver_.Poll(close.Value());
                        if (completion.HasError())
                            std::terminate();
                        if (completion.Value()) {
                            if (completion.Value()->operation != close.Value() ||
                                !std::holds_alternative<Audio::Backend::Closed>(completion.Value()->outcome) ||
                                driver_.AcknowledgeCompletion(close.Value()).HasError())
                                std::terminate();
                            return;
                        }
                        if (driver_.AdvanceControl().HasError())
                            std::terminate();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    std::terminate();
                } catch (...) {
                    std::terminate();
                }
            }

            /** @brief Borrow the driver pinned by output until adoption. @return Selected driver. */
            DrivenOutput &Driver() noexcept {
                return driver_;
            }

            Audio::Internal::AudioFrontendOutput output;

        private:
            DrivenOutput &driver_;
        };

        /** @brief Captures one closed-peer catalog through the actual bounded native protocol. */
        Result<Audio::AudioDeviceSnapshot> Discover(DrivenOutput &backend) {
            auto operation = backend.Begin(Audio::Backend::Enumerate{}, Deadline());
            if (operation.HasError())
                return Result<Audio::AudioDeviceSnapshot>::Failure(operation.ErrorValue());
            PreparationOperation pending{backend, operation.Value()};
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

        /** @brief Complete one pre-callback Open with a bounded control-only pump; caller retains and acknowledges the receipt. */
        Result<Audio::Backend::Completion> CompletePreparation(DrivenOutput &backend, const Audio::Backend::OperationId operation,
                                                               const Audio::AudioMonotonicTimestamp deadline) {
            const auto end = std::chrono::steady_clock::time_point{
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(deadline.nanoseconds))};
            while (std::chrono::steady_clock::now() < end) {
                auto polled = backend.Poll(operation);
                if (polled.HasError())
                    return Result<Audio::Backend::Completion>::Failure(polled.ErrorValue());
                if (polled.Value())
                    return Result<Audio::Backend::Completion>::Success(*polled.Value());
                if (auto advanced = backend.AdvanceControl(); advanced.HasError())
                    return Result<Audio::Backend::Completion>::Failure(advanced.ErrorValue());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return Result<Audio::Backend::Completion>::Failure(
                MakeError(Audio::AudioErrors::BackendFailed, "Preview Open deadline expired"));
        }

        /** @brief Detached prepared output and its control driver, adopted together only after composition succeeds. */
        struct PreparedLane final {
            std::unique_ptr<Audio::AudioFrontend> frontend;
            std::function<Result<void>()> advance;
        };

        /** @brief Opens the selected output on control, then prepares actual-format playback before any callback attaches. */
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
            PreparedOutput candidate{std::make_unique<DrivenOutput>(std::move(selectedOutput))};
            auto catalog = Discover(candidate.Driver());
            if (catalog.HasError())
                return Result<PreparedLane>::Failure(catalog.ErrorValue());
            if (!Audio::ValidateAudioDeviceSnapshot(catalog.Value()) || catalog.Value().owner != owner ||
                catalog.Value().backend != candidate.Driver().Kind())
                return Result<PreparedLane>::Failure(MakeError(Audio::AudioErrors::PlaybackRequestInvalid));
            const auto selected = Audio::ResolveAudioDevice(catalog.Value(), Audio::AudioDefaultDeviceRole::Multimedia);
            if (selected.status != Audio::AudioDeviceResolutionStatus::Resolved)
                return Result<PreparedLane>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
            const Audio::AudioDeviceEpoch epoch{selected.device, 1, 1};
            auto *driver = &candidate.Driver();
            std::function<Result<void>()> advance = [driver] {
                return driver->Advance();
            };
            candidate.output.open = {epoch,
                                     {Audio::AudioDefaultDeviceRole::Multimedia,
                                      {48'000, Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Stereo)},
                                      {{44'100, Audio::MakeAudioSpeakerLayout(Audio::AudioSpeakerPreset::Stereo)}},
                                      {16, 256, 4096}}};
            candidate.output.devices = std::move(catalog).Value();
            candidate.output.clockDomain = 1;
            const auto deadline = Deadline();
            auto opened = driver->Begin(candidate.output.open, deadline);
            if (opened.HasError())
                return Result<PreparedLane>::Failure(opened.ErrorValue());
            PreparationOperation pending{*driver, opened.Value()};
            auto completion = CompletePreparation(*driver, opened.Value(), deadline);
            if (completion.HasError())
                return Result<PreparedLane>::Failure(completion.ErrorValue());
            if (const auto *failed = std::get_if<Audio::Backend::Failed>(&completion.Value().outcome))
                return Result<PreparedLane>::Failure(failed->error);
            const auto *actual = std::get_if<Audio::Backend::Opened>(&completion.Value().outcome);
            if (!actual || completion.Value().operation != opened.Value())
                return Result<PreparedLane>::Failure(MakeError(Audio::AudioErrors::EventQueueInvalid));
            if (Audio::ValidateAudioDeviceNegotiation(candidate.output.open.format, candidate.output.devices, actual->format).status !=
                    Audio::AudioDeviceNegotiationStatus::Accepted ||
                actual->access != candidate.output.open.access || actual->timing.epoch != epoch ||
                actual->timing.capturedAt.clockDomain != candidate.output.clockDomain ||
                actual->format.formatRevision != epoch.formatRevision ||
                !Audio::ValidateAudioDeviceTimingReport(actual->timing, epoch, driver->Kind()))
                return Result<PreparedLane>::Failure(MakeError(Audio::AudioErrors::DeviceFormatUnsupported));
            auto preparation =
                AudioPreviewHostDetail::Prepare(request, decoded.Value(), owner, actual->format.effective, actual->format.callbackFrames);
            if (preparation.HasError())
                return Result<PreparedLane>::Failure(preparation.ErrorValue());
            auto prepared = std::move(preparation).Value();
            candidate.output.openOperation = opened.Value();
            auto frontend = Audio::Internal::AudioFrontendComposition::Create(prepared, candidate.output);
            if (frontend.HasError()) {
                AudioPreviewHostDetail::RetireDetached(prepared);
                return Result<PreparedLane>::Failure(frontend.ErrorValue());
            }
            pending.Adopted();
            return Result<PreparedLane>::Success({std::move(frontend).Value(), std::move(advance)});
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

    /** @copydoc SelectEditorAudioPreviewOutput */
    Result<EditorAudioPreviewOutputFactory> SelectEditorAudioPreviewOutput(const Audio::AudioBackendKind backend) {
        using FactoryResult = Result<EditorAudioPreviewOutputFactory>;
        using enum Audio::AudioBackendKind;
        switch (backend) {
            case SDL3Audio:
                // Preserve the existing optional-preview contract when SDL audio is disabled in a GUI build.
                return FactoryResult::Success(MakeSdl3AudioPreviewOutput);
            case NullAudio:
                return FactoryResult::Success(MakeNullAudioPreviewOutput);
            case WASAPI:
#if defined(HORO_EDITOR_AUDIO_WASAPI) && HORO_EDITOR_AUDIO_WASAPI
                return FactoryResult::Success([](const Audio::AudioRuntimeId owner) {
                    return NativeOutput(Audio::Backend::CreateWasapiAudioBackend({owner, 1}));
                });
#else
                break;
#endif
            case CoreAudio:
#if defined(HORO_EDITOR_AUDIO_COREAUDIO) && HORO_EDITOR_AUDIO_COREAUDIO
                return FactoryResult::Success([](const Audio::AudioRuntimeId owner) {
                    return NativeOutput(Audio::Backend::CreateCoreAudioBackend({owner, 1}));
                });
#else
                break;
#endif
            case PipeWire:
#if defined(HORO_EDITOR_AUDIO_PIPEWIRE) && HORO_EDITOR_AUDIO_PIPEWIRE
            {
                auto initialized = Audio::Backend::InitializePipeWireAudioLibrary();
                if (initialized.HasError())
                    return FactoryResult::Failure(initialized.ErrorValue());
                return FactoryResult::Success([library = std::move(initialized).Value()](const Audio::AudioRuntimeId owner) {
                    return NativeOutput(Audio::Backend::CreatePipeWireAudioBackend({owner, 1}, library));
                });
            }
#else
                break;
#endif
            default:
                return FactoryResult::Failure(MakeError(Audio::AudioErrors::IdentityInvalid));
        }
        return FactoryResult::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
    }

    struct EditorAudioPreviewHost::State final {
        Audio::Backend::AudioRuntimeOwnerIssuer &owners;
        AudioComponentPreviewSnapshot snapshot;
        std::unique_ptr<Audio::AudioFrontend> frontend;
        EditorAudioPreviewOutputFactory factory;
        std::function<Result<void>()> advance;  // Borrows only the backend retained by frontend; cleared after Closed.
        bool startVoice{};

        explicit State(Audio::Backend::AudioRuntimeOwnerIssuer &issuer) : owners(issuer) {}
    };

    /** @copydoc EditorAudioPreviewHost::EditorAudioPreviewHost */
    EditorAudioPreviewHost::EditorAudioPreviewHost(Audio::Backend::AudioRuntimeOwnerIssuer &owners,
                                                   EditorAudioPreviewOutputFactory outputFactory)
        : state_(std::make_unique<State>(owners)) {
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
        const auto owner = state_->owners.Issue();
        if (owner.HasError())
            return fail(owner.ErrorValue());
        auto prepared = PrepareLane(request, owner.Value(), state_->factory);
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
