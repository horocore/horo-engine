#pragma once
#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Editor/AudioComponentPreview.h"

#include <functional>

namespace Horo::Editor {
    /** @brief One explicitly selected private output peer and its owning control driver. */
    struct EditorAudioPreviewBackend final {
        std::unique_ptr<Audio::Backend::AudioBackend> output;
        std::function<Result<void>()> advance;
    };

    /** @brief Application output preparation policy; it never runs inside widget drawing. */
    using EditorAudioPreviewOutputFactory = std::function<Result<EditorAudioPreviewBackend>(Audio::AudioRuntimeId)>;
    /** @brief Selects the SDL3 peer explicitly; absent build support returns ProviderUnavailable, never Null. */
    [[nodiscard]] Result<EditorAudioPreviewBackend> MakeSdl3AudioPreviewOutput(Audio::AudioRuntimeId owner);
    /** @brief Selects the actual deterministic Null peer explicitly for headless hosts. */
    [[nodiscard]] Result<EditorAudioPreviewBackend> MakeNullAudioPreviewOutput(Audio::AudioRuntimeId owner);

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
