#pragma once

/** @file EditorAudioPreviewControlHost.h @brief Owned Audio control-thread bridge for graphical clients with another native apartment. */

#include "EditorAudioPreviewHost.h"

#include <memory>

namespace Horo::Editor {
    /**
     * @brief UI-owner capability with a dedicated owner for frontend/backend control and facade lifetime.
     *
     * Start copies one immutable request and admits preparation asynchronously. UI Pump/Snapshot read
     * published values without waiting for native work. Stop is a generation-tagged critical intent;
     * it cannot be lost to a busy publication lock. Native render code never acquires bridge locks.
     * The factory and all of its borrowed dependencies must remain valid through explicit shutdown.
     * Destruction closes admission, retires the inner host on its owner, then joins the actual thread.
     * A five-second shutdown timeout terminates without freeing the unproven ownership island.
     * All public calls and destruction stay on one UI/client owner; only copied values cross to control.
     */
    class EditorAudioPreviewControlHost final : public IAudioComponentPreview {
    public:
        /** @brief Create one explicit control owner without selecting/probing a backend.
         * @param owners Process-root authority which outlives the bridge and its joined worker.
         * @param factory Process-selected factory; invoked only on the new control owner.
         * @return Owned bridge or typed missing/memory/thread-creation failure, with no orphan thread.
         */
        [[nodiscard]] static Result<std::unique_ptr<EditorAudioPreviewControlHost>> Create(Audio::Backend::AudioRuntimeOwnerIssuer &owners,
                                                                                           EditorAudioPreviewOutputFactory factory);
        /** @brief Retire and join on the UI owner; a timeout never authorizes detached thread or native destruction. */
        ~EditorAudioPreviewControlHost() override;
        EditorAudioPreviewControlHost(const EditorAudioPreviewControlHost &) = delete;
        EditorAudioPreviewControlHost &operator=(const EditorAudioPreviewControlHost &) = delete;

        /** @copydoc IAudioComponentPreview::Start */
        [[nodiscard]] Result<void> Start(const AudioComponentPreviewRequest &request) override;
        /** @copydoc IAudioComponentPreview::Stop */
        void Stop(Assets::AssetId workspace) noexcept override;
        /** @copydoc IAudioComponentPreview::Pump */
        [[nodiscard]] Result<void> Pump() override;
        /** @copydoc IAudioComponentPreview::Snapshot */
        [[nodiscard]] AudioComponentPreviewSnapshot Snapshot() const override;

    private:
        struct State;
        /** @brief Pin complete exchange storage before starting its owned control thread. */
        explicit EditorAudioPreviewControlHost(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
