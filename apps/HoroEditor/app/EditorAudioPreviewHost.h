#pragma once
#include "Horo/Audio/AudioFrontend.h"
#include "Horo/Audio/AudioStreamingService.h"
#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Editor/AudioComponentPreview.h"

#include <functional>

namespace Horo::Editor {
    /** @brief One explicitly selected private output peer and its owning control driver. */
    struct EditorAudioPreviewBackend final {
        std::unique_ptr<Audio::Backend::AudioBackend> output;
        std::function<Result<void>()> advanceControl; /**< Executes only an accepted nonterminal operation. */
        std::function<Result<void>()>
            advanceCallback; /**< Optional explicit headless callback clock; native SDL3 drives its own callbacks. */
    };

    /** @brief Application output preparation policy; it never runs inside widget drawing. */
    using EditorAudioPreviewOutputFactory = std::function<Result<EditorAudioPreviewBackend>(Audio::AudioRuntimeId)>;
    /** @brief Selects the SDL3 peer explicitly; absent build support returns ProviderUnavailable, never Null. */
    [[nodiscard]] Result<EditorAudioPreviewBackend> MakeSdl3AudioPreviewOutput(Audio::AudioRuntimeId owner);
    /** @brief Selects the actual deterministic Null peer explicitly for headless hosts. */
    [[nodiscard]] Result<EditorAudioPreviewBackend> MakeNullAudioPreviewOutput(Audio::AudioRuntimeId owner);

    /** @brief Owns one discovered output and its normal editor mixer lane for explicit local listening. */
    class EditorAudioMonitorOutput final {
    public:
        /** @brief Enumerates the retained closed peer through its ordinary operation protocol. */
        [[nodiscard]] static Result<std::unique_ptr<EditorAudioMonitorOutput>> Discover(EditorAudioPreviewBackend backend);
        ~EditorAudioMonitorOutput();

        /** @brief Returns the session-local output identities and display names from the retained peer. */
        [[nodiscard]] std::span<const Audio::AudioDiscoveredDevice> Outputs() const noexcept;
        /** @brief Reports whether this exact discovered device can be pinned by this closed peer. */
        [[nodiscard]] bool CanSelect(Audio::AudioDeviceId device) const noexcept;
        /** @brief Opens exactly the confirmed private endpoint with a preadmitted, empty live source.
         * @param device Selected non-migrating output from this session. @param service Shared stream owner.
         * @param stream Dedicated microphone PCM input. @param hostLease Pins dependencies of the service.
         * @return Exact mixer bus for capture admission, or a typed failure.
         */
        [[nodiscard]] Result<Audio::AudioBusHandle> Start(Audio::AudioDeviceId device,
                                                         const std::shared_ptr<Audio::AudioStreamingService> &service,
                                                         Audio::AudioStreamHandle stream, std::shared_ptr<const void> hostLease);
        /** @brief Drives at most one output-control step and publishes frontend state. */
        [[nodiscard]] Result<void> Pump();
        /** @brief Starts the monitor voice after capture has bound the exact live stream. */
        [[nodiscard]] Result<void> StartVoice();
        /** @brief Requests asynchronous retirement of this output and its voice. */
        void Close() noexcept;
        /** @brief Returns the output's current lifecycle, epoch and voice facts. */
        [[nodiscard]] Audio::AudioFrontendSnapshot Snapshot() const;

    private:
        struct State;
        explicit EditorAudioMonitorOutput(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    /** @brief Application composition root for an explicitly selected SDL3 inspector preview peer. */
    class EditorAudioPreviewHost final : public IAudioComponentPreview {
    public:
        explicit EditorAudioPreviewHost(EditorAudioPreviewOutputFactory outputFactory);
        ~EditorAudioPreviewHost() override;
        [[nodiscard]] Result<void> Start(const AudioComponentPreviewRequest &request) override;
        void Stop(Assets::AssetId workspace) noexcept override;
        [[nodiscard]] Result<void> Pump() override;
        [[nodiscard]] AudioComponentPreviewSnapshot Snapshot() const override;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
