#pragma once
#include "Horo/Audio/Internal/AudioBackend.h"
#include "Horo/Audio/Internal/AudioRuntimeOwnerIssuer.h"
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

    /** @brief Capture one peer explicitly at process composition; never probe or substitute another peer.
     * @param backend Fixed process preview selection, independent of game output and renderer selection.
     * @return Retained factory or typed unavailable/invalid failure. PipeWire selection explicitly initializes
     * one process library lease; the factory shares it across all of its output generations.
     * SDL retains its existing deferred ProviderUnavailable result when omitted from the build, so the
     * default optional preview does not prevent a GUI build without audio from starting.
     * @pre Call once on process composition for this output family. All calls on each returned output stay on
     * its declared control owner; WASAPI currently requires MTA control, including in a graphical host.
     */
    [[nodiscard]] Result<EditorAudioPreviewOutputFactory> SelectEditorAudioPreviewOutput(Audio::AudioBackendKind backend);

    /** @brief Application composition root for one explicitly selected inspector preview peer. */
    class EditorAudioPreviewHost final : public IAudioComponentPreview {
    public:
        /** @brief Borrow the process identity authority and own the explicitly selected preview policy.
         * @param owners Process-root authority shared with all independent Audio output families; outlives this host.
         * @param outputFactory Fixed output policy; receives a fresh identity for each attempted runtime generation.
         */
        EditorAudioPreviewHost(Audio::Backend::AudioRuntimeOwnerIssuer &owners, EditorAudioPreviewOutputFactory outputFactory);
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
